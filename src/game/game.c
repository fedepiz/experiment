////////////////////////////////
//~ fp: Game Layer -- the code of the layers below, in the order of their use

#include "game/game.h"
#include "base/arena.h"
#include "base/core.h"
#include "base/math.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/board.h"
#include "game/thing_db.h"
#include "game/worldgen.h"
#include "gfx/color.h"
#include "tabula.h"

// The ways of life are below, next to the phases that read them. Setup needs
// only the lookup by name.
internal I32 gm__way_of_life_find(String8 key);

////////////////////////////////
//~ fp: Game

// Copy the fields of the database into the board: the terrain ids, the
// feature masks, and the travel rules, whose costs come from the terrain
// table. Then drop the path cache. Call this after each write to a field.
StaticAssert(WG_TERRAIN_CAP <= BD_TERRAIN_CAP, gm_terrain_costs_fit);
internal void gm__board_mirror(GM_Game* game, WG_Params* params) {
  TH_Db* db = game->db;
  BD_Board* board = game->board;
  for(I32 y = 0; y < board->height; y += 1) {
    for(I32 x = 0; x < board->width; x += 1) {
      V2I p = {x, y};
      BD_Tile* tile = bd_tile_at(board, p);
      tile->terrain = (BD_Terrain)th_ifield_get(db, p, TH_IField_Terrain);
      for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
        tile->features[feature] = (U8)th_ifield_get(db, p, GM_FEATURE_IFIELDS[feature]);
      }
    }
  }
  BD_TravelRules* rules = &board->rules;
  for(U32 type = 0; type < WG_TERRAIN_TYPE_COUNT; type += 1) {
    rules->terrain_cost[type] = WG_TERRAIN_TYPES[type].move_cost;
  }
  rules->terrain_cost_count = WG_TERRAIN_TYPE_COUNT;
  rules->road_cost = params->road_cost;
  rules->river_cross_cost = params->river_cross_cost;
  bd_path_cache_clear(board);
}

// A road for a demonstration. It joins two points on opposite sides of the
// continent, and it follows the best path across the terrain. The function
// finds the path on the board, writes the road into the fields, then copies
// the fields into the board again.
internal void gm__demo_road(GM_Game* game, WG_Params* params) {
  BD_Board* board = game->board;
  V2I west = bd_snap_passable(board, (V2I){board->width / 6, board->height / 2});
  V2I east = bd_snap_passable(board, (V2I){board->width * 5 / 6, board->height / 2});
  ArenaTemp scratch = arena_get_scratch(0, 0);
  BD_Path path = bd_path_find(scratch.arena, board, west, east);
  for(U64 idx = 0; idx + 1 < path.count; idx += 1) {
    Dir4 dir = dir4_from_delta(v2i_sub(path.points[idx + 1], path.points[idx]));
    wg_field_connect(game->db, path.points[idx], dir, TH_IField_RoadMask);
  }
  arena_release_scratch(scratch);
  gm__board_mirror(game, params);
}

internal V2I gm__xy_get(TH_Db* db, TH_Id id) {
  V2I pos = {0};
  pos.x = th_ivar_get(db, id, TH_IVar_X);
  pos.y = th_ivar_get(db, id, TH_IVar_Y);
  return pos;
}

internal void gm__xy_set(TH_Db* db, TH_Id id, V2I pos) {
  th_ivar_set(db, id, TH_IVar_X, pos.x);
  th_ivar_set(db, id, TH_IVar_Y, pos.y);
}

////////////////////////////////
//~ fp: Tile Queries
//
// A tile query finds the tiles near a point that pass a set of filters. Use it
// to choose a site: where a group can settle, and where it can go.

typedef struct {
  V2I coords;
} GM_TileHit;

typedef struct {
  GM_TileHit* tiles;
  U32 count;
} GM_TilesHits;

typedef U16 GM_QueryTilesFlags;

enum {
  GM_QueryTilesFlag_Unclaimed = (1 << 0),
};

typedef struct {
  V2I focus;
  I32 radius_min;
  I32 radius_max;
  WG_TerrainFlags terrain_flags_any;
  WG_TerrainFlags terrain_flags_all;
  GM_QueryTilesFlags query_flags_all;
} GM_QueryTiles;


internal GM_QueryTiles gm__query_tiles_make(V2I focus, I32 range) {
  GM_QueryTiles query = {0};
  query.focus = focus;
  query.radius_max = range;
  return query;
}

internal GM_TilesHits gm__query_tiles_run(Arena* arena, GM_Game* game, GM_QueryTiles query) {
  BD_Board* board = game->board;
  TH_Db* db = game->db;
  GM_TilesHits out = {0};

  // The walk computes the extents, the distance test and the empty cases, so
  // this function holds the filters of the query and nothing more.
  BD_Disc disc = bd_disc_ring(board, query.focus, (F32)query.radius_min, (F32)query.radius_max);
  U64 hits_cap = bd_disc_bound(disc);
  if(hits_cap == 0) { return out; }
  out.tiles = push_array_no_zero(arena, GM_TileHit, hits_cap);

  while(bd_disc_next(&disc)) {
    V2I pos = disc.pos;
    BD_Tile* tile = bd_tile_at(board, pos);
    const WG_TerrainType* terrain_type = wg_terrain_type_get(tile->terrain);

    if(query.terrain_flags_any != 0 && (terrain_type->flags & query.terrain_flags_any) == 0) {
      continue;
    }

    if((terrain_type->flags & query.terrain_flags_all) != query.terrain_flags_all) {
      continue;
    }

    if(query.query_flags_all & GM_QueryTilesFlag_Unclaimed) {
      if(th_field_ref_get(db, TH_FieldRef_Home, pos) != 0) continue;
    }

    Assert(out.count < hits_cap);
    GM_TileHit* hit = &out.tiles[out.count++];
    hit->coords = pos;
  }
  return out;
}

// Extraction writes the facts of the database into the layers that come from
// it. It writes them in full, on each tick. Today the board is the only such
// layer. A later index, roster or set of display data goes into the same
// single pass over the things, and reads the facts that it needs. No other
// code writes those layers.
internal void gm__extract(GM_Game* game) {
  TH_Db* db = game->db;

  //- fp: the board, from the side of the pawns first. The sweep examines
  //  every pawn, so it removes each pawn whose thing is gone or is no longer
  //  on the board.
  ArenaTemp scratch = arena_get_scratch(0, 0);
  BD_PawnArray pawns = bd_pawns_all(scratch.arena, game->board);
  for(U64 i = 0; i < pawns.count; i += 1) {
    // An old id reads no flag, so a thing that is gone fails this test too.
    TH_Id id = (TH_Id)pawns.elems[i]->key;
    if(!th_flag_get(db, id, TH_Flag_Placed)) {
      bd_pawn_remove(game->board, id);
    }
  }
  arena_release_scratch(scratch);

  //- fp: the side of the things, which is the one loop that each extraction
  //  shares. A thing on the board writes the position of its pawn here, and
  //  makes that pawn when it does not exist. Only this loop can make a pawn,
  //  because a sweep of the pawns never reaches a pawn that is absent.
  for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
    if(th_flag_get(db, this, TH_Flag_Placed)) {
      bd_pawn_place(game->board, this, gm__xy_get(db, this));
    }
  }
}

internal void gm_init(Arena* arena, GM_Game* game, U64 seed) {
  MemoryZeroStruct(game);
  game->db = th_init_db(arena);

  //- fp: the world. Read the terrain table and the parameters from the files,
  //  generate the world into the fields, then copy the fields into the board.
  wg_terrain_table_load(str8_lit("data/terrain_types.tabula"));
  WG_Params params = wg_params_load(str8_lit("data/world.tabula"));
  wg_generate(game->db, &params, seed);
  game->board = bd_board_alloc(arena, params.width, params.height, 1024);
  gm__board_mirror(game, &params);
  gm__demo_road(game, &params);

  TH_Db* db = game->db;

  // Waypoints near the corners. Each one moves to the nearest land that this
  // world grew there. The Next ref joins them into a loop.
  V2I corners[] = {{30, 30}, {220, 40}, {210, 210}, {40, 220}};
  TH_Id waypoints[ArrayCount(corners)] = {0};
  for(U32 i = 0; i < ArrayCount(corners); i += 1) {
    TH_Id id = th_spawn(db);
    gm__xy_set(db, id, bd_snap_passable(game->board, corners[i]));
    waypoints[i] = id;
  }

  for(U32 i = 0; i < ArrayCount(waypoints); i += 1) {
    th_ref_set(db, TH_Ref_Next, waypoints[i], waypoints[(i + 1) % ArrayCount(waypoints)]);
  }

  for(U32 i = 0; i < 3; i += 1) {
    TH_Id id = th_spawn(db);
    gm__xy_set(db, id, gm__xy_get(db, waypoints[i]));
    th_ref_set(db, TH_Ref_Goal, id, waypoints[(i + 1) % ArrayCount(waypoints)]);
    th_ivar_set(db, id, TH_IVar_Sprite, GM_Sprite_Wagon);
    th_flag_set(db, id, TH_Flag_Placed, true);
    th_flag_set(db, id, TH_Flag_Mobile, true);
  }

  typedef struct {
    String8 name;
    V2I pos;
    GM_Sprite sprite;
    String8 way_of_life;
    F32 population;
  } GroupSpawn;

  const GroupSpawn GROUP_SPAWNS[] = {
      // Much less than the land supports. The group therefore grows into its
      // range, and does not start at the limit with no change to show.
      {str8_lit_comp("Group #A"), {102, 96}, GM_Sprite_Band, str8_lit_comp("hunter_gatherer"), .population = 50},
  };

  for(U32 idx = 0; idx < ArrayCount(GROUP_SPAWNS); ++idx) {
    const GroupSpawn* spawn = &GROUP_SPAWNS[idx];
    TH_Id id = th_spawn(db);
    TH_Phrase* name = th_label(db, id, TH_Label_Name);
    th_push_word(name, th_define_word(db, spawn->name));
    gm__xy_set(db, id, spawn->pos);
    th_var_set(db, id, TH_Var_Population, spawn->population);
    th_ivar_set(db, id, TH_IVar_Sprite, spawn->sprite);
    th_flag_set(db, id, TH_Flag_HasInfluence, true);
    th_flag_set(db, id, TH_Flag_Placed, true);

    I32 way = gm__way_of_life_find(spawn->way_of_life);
    Assert(way > 0); // an unknown name is a mistake in the spawn table
    th_ivar_set(db, id, TH_IVar_WayOfLife, way);
  }

  th_commit(db);
  gm__extract(game);
  game->initialised = true;
}

////////////////////////////////
//~ fp: Selection

internal void gm_select(GM_Game* game, V2I tile) {
  GM_Selection selection = {0};
  if(bd_in_bounds(game->board, tile)) {
    selection.kind = GM_SelectionKind_Tile;
    selection.tile = tile;
    BD_Pawn* pawn = bd_tile_at(game->board, tile)->first_pawn;
    if(pawn != 0) {
      selection.kind = GM_SelectionKind_Thing;
      selection.id = (TH_Id)pawn->key;
    }
  }
  game->selection = selection;
}

internal void gm_deselect(GM_Game* game) {
  game->selection = (GM_Selection){0};
}

// Read the tile of a selected thing from that thing again. A thing that is
// gone, and a thing that is no longer on the board, clear the selection. This
// runs at each update.
internal void gm__selection_refresh(GM_Game* game) {
  if(game->selection.kind == GM_SelectionKind_Thing) {
    if(th_flag_get(game->db, game->selection.id, TH_Flag_Placed)) {
      game->selection.tile = gm__xy_get(game->db, game->selection.id);
    } else {
      game->selection = (GM_Selection){0};
    }
  }
}

internal String8 gm__thing_name(Arena* arena, TH_Db* db, TH_Id id) {
  return th_resolve_phrase(arena, db, *th_label(db, id, TH_Label_Name), str8_lit(""));
}

internal void gm__tile_facts(Arena* arena, GM_Game* game, TB_Value* out, V2I pos) {
  TH_Db* db = game->db;
  TB_Value* xy = tb_add_list(arena, out, str8_lit("pos"));
  tb_list_push_num(arena, xy, (F32)pos.x);
  tb_list_push_num(arena, xy, (F32)pos.y);

  U32 terrain = (U32)th_ifield_get(db, pos, TH_IField_Terrain);
  tb_add_str8(arena, out, str8_lit("terrain"), wg_terrain_name(terrain));
  if(terrain < WG_TERRAIN_TYPE_COUNT) {
    tb_add_num(arena, out, str8_lit("move_cost"), WG_TERRAIN_TYPES[terrain].move_cost);
  }

  BD_Tile* tile = bd_tile_at(game->board, pos);
  TB_Value* features = 0;
  for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
    if(tile->features[feature] == 0) { continue; }
    // Make the list at the first feature, so a tile with no feature carries
    // no empty list.
    if(features == 0) { features = tb_add_list(arena, out, str8_lit("features")); }
    tb_list_push_str8(arena, features, GM_FEATURE_NAMES[feature]);
  }

  TH_Id home = th_field_ref_get(db, TH_FieldRef_Home, pos);
  if(home != 0) {
    String8 name = gm__thing_name(arena, db, home);
    if(name.size > 0) {
      tb_add_str8(arena, out, str8_lit("home"), name);
    }
  }
}

internal void gm__selection_info(Arena* arena, GM_Game* game, TB_Value* root) {
  TH_Db* db = game->db;
  GM_Selection selection = game->selection;
  if(selection.kind == GM_SelectionKind_Nil) { return; }
  TB_Value* info = tb_add_object(arena, root, str8_lit("selection"));
  switch(selection.kind) {
    case GM_SelectionKind_Tile: {
      tb_add_str8(arena, info, str8_lit("kind"), str8_lit("tile"));
      tb_add_num(arena, info, str8_lit("crops"), th_field_get(db, selection.tile, TH_Field_Crops));
      tb_add_num(arena, info, str8_lit("grass"), th_field_get(db, selection.tile, TH_Field_Grass));
      tb_add_num(arena, info, str8_lit("wildlife"), th_field_get(db, selection.tile, TH_Field_Wildlife));
    } break;
    case GM_SelectionKind_Thing: {
      tb_add_str8(arena, info, str8_lit("kind"), str8_lit("thing"));
      GM_Sprite sprite = (GM_Sprite)Clamp(0, th_ivar_get(db, selection.id, TH_IVar_Sprite), GM_Sprite_COUNT - 1);
      String8 name = gm__thing_name(arena, db, selection.id);
      if(name.size == 0) { name = GM_SPRITE_NAMES[sprite]; }
      tb_add_str8(arena, info, str8_lit("name"), name);
      tb_add_str8(arena, info, str8_lit("sprite"), GM_SPRITE_NAMES[sprite]);
      tb_add_num(arena, info, str8_lit("population"), th_var_get(db, selection.id, TH_Var_Population));
      tb_add_num(arena, info, str8_lit("food"), th_var_get(db, selection.id, TH_Var_FoodStore));
    } break;
  }
  gm__tile_facts(arena, game, tb_add_object(arena, info, str8_lit("tile")), selection.tile);
}

internal TB_Value* gm_info(Arena* arena, GM_Game* game) {
  TB_Value* info = tb_build_object(arena);
  gm__selection_info(arena, game, info);
  return info;
}

////////////////////////////////
//~ fp: Ways Of Life
//
// A way of life is how a group turns land into people. It is a row of
// numbers. The economy phases read the row. Two ways of life differ in their
// numbers only, so no phase has a branch on a way of life.
//
// Row 0 is the nil way of life. A group with row 0 takes nothing.

typedef struct {
  String8 key;    // the name that the spawn table uses
  TH_Field stock; // the field that the group takes from the land
  F32 labour;     // stock that one person gathers in one tick
  F32 yield;      // food for each unit of stock
  F32 reach;      // tiles that the group claims around itself
  // Ticks of food that the group keeps in hand, above the food of this tick.
  // The group gathers enough to eat and to fill this reserve. A group with a
  // reserve of 0 keeps no food, and a bad tick is a famine at once.
  F32 reserve;
} GM_WayOfLife;

global const GM_WayOfLife WAYS_OF_LIFE[] = {
    {0},
    {str8_lit_comp("hunter_gatherer"), TH_Field_Wildlife, 2, 1, 8, 1},
};

internal const GM_WayOfLife* gm__way_of_life_get(TH_Db* db, TH_Id id) {
  U32 idx = (U32)th_ivar_get(db, id, TH_IVar_WayOfLife);
  Assert(idx < ArrayCount(WAYS_OF_LIFE));
  return &WAYS_OF_LIFE[idx];
}

// The row with this name. Row 0 shows that the name is unknown.
internal I32 gm__way_of_life_find(String8 key) {
  I32 result = 0;
  for(U32 i = 1; i < ArrayCount(WAYS_OF_LIFE); i += 1) {
    if(str8_match(WAYS_OF_LIFE[i].key, key, 0)) {
      result = (I32)i;
      break;
    }
  }
  return result;
}

#define GM_TICK_DT       0.1f // seconds of sim per tick; every rate below is per-tick
#define GM_TICK_BANK_MAX 0.5f // at most 5 banked ticks replay after a stall

internal void gm_update(GM_Game* game, F32 dt) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  TH_Db* db = game->db;
  dt *= game->paused ? 0 : 1;
  game->move_timer = ClampTop(game->move_timer + dt, GM_TICK_BANK_MAX);
  while(game->move_timer > GM_TICK_DT) {
    game->tick_num++;
    game->move_timer -= GM_TICK_DT;

    // The tick, in phases. Each phase reads every thing that it concerns
    // before the next phase starts. Inside a phase the things are therefore
    // independent, and the order of the slots cannot decide a result.
    //
    // Each phase states what it reads and what it writes. A phase that reads
    // across the things what it also writes across the things would break the
    // rule above. No phase does that.

    BD_PartitionCell* influence_map = 0;

    //- fp: Regrow -- reads: terrain; writes: stocks
    // Each stock of a tile moves toward the ceiling of its terrain type, at
    // the rate of that terrain type. A terrain type with a rate of 0 grows
    // nothing, and a terrain type with a ceiling of 0 holds nothing.
    {
      V2I world_size = th_world_size(db);
      for(I32 y = 0; y < world_size.y; ++y) {
        for(I32 x = 0; x < world_size.x; ++x) {
          V2I pos = {x, y};
          const WG_TerrainType* type = wg_terrain_type_get((U32)th_ifield_get(db, pos, TH_IField_Terrain));
          for(WG_Stock stock = 0; stock < WG_Stock_COUNT; stock += 1) {
            F32* field = th_field(db, pos, WG_STOCK_FIELDS[stock]);
            *field = Min(*field + type->stock_renew_rate[stock], type->stock_max[stock]);
          }
        }
      }
    }

    //- fp: Claim -- reads: pawn positions; writes: influence_map, Home
    {
      BD_Board* board = game->board;
      const F32 STRENGTH = 1.0f;
      const BD_Falloff FALLOFF = BD_Falloff_Linear;
      // Count, then fill. Two small passes are less work than storage that
      // grows. A source that is not on the board finds no pawn and claims
      // nothing.
      BD_SourceArray sources = {0};
      for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
          this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
        sources.count += 1;
      }
      sources.elems = push_array_no_zero(scratch.arena, BD_Source, sources.count);
      U64 at = 0;
      for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
          this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
        const GM_WayOfLife* wol = gm__way_of_life_get(db, this);
        sources.elems[at] = (BD_Source){
            .key = this, // a placed thing is its own pawn's key
            .strength = STRENGTH,
            .range = wol->reach,
            .falloff = FALLOFF,
        };
        at += 1;
      }

      // The nil id is the unassigned key. th_field_ref_set reads it as an
      // instruction to clear the tile.
      influence_map = bd_partition(scratch.arena, board, sources, 0);
      for(I32 y = 0; y < board->height; y += 1) {
        for(I32 x = 0; x < board->width; x += 1) {
          TH_Id home = (TH_Id)influence_map[(U64)y * board->width + x].key;
          th_field_ref_set(db, TH_FieldRef_Home, (V2I){x, y}, home);
        }
      }
    }

    //- fp: Produce -- reads: influence_map, stocks; writes: stocks, food store
    // The partition gives each tile to one group, so two groups never draw
    // from one tile. The order of the groups therefore does not change the
    // result.
    for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
        this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
      F32 population = th_var_get(db, this, TH_Var_Population);
      F32 store = th_var_get(db, this, TH_Var_FoodStore);
      V2I my_pos = gm__xy_get(db, this);
      const GM_WayOfLife* wol = gm__way_of_life_get(db, this);
      BD_Disc disc = bd_disc(game->board, my_pos, wol->reach);

      //- fp: pass one: the stock that grows on the tiles of this group
      F32 available = 0.0;
      for(BD_Disc iter = disc; bd_disc_next(&iter);) {
        V2I cell = iter.pos;
        B32 owned = influence_map[(U32)(cell.y * game->board->width + cell.x)].key == this;
        if(!owned) { continue; }
        available += th_field_get(db, cell, wol->stock);
      }

      // The group wants food for this tick and for its reserve. It counts the
      // food that it holds already, so a full store stops the work. Three
      // quantities limit the result: the food that the group wants, the hands
      // that it has, and the stock that grows on its land.
      F32 need = ClampBot(population * (1.0f + wol->reserve) - store, 0.0f);
      F32 hands = population * wol->labour;
      F32 target = Min(need / wol->yield, hands);
      F32 take_fraction = available > 0 ? Min(1.0f, target / available) : 0;

      //- fp: pass two: the same part of every tile, so a rich tile gives more
      F32 got = 0;
      for(BD_Disc iter = disc; bd_disc_next(&iter);) {
        V2I cell = iter.pos;
        B32 owned = influence_map[(U32)(cell.y * game->board->width + cell.x)].key == this;
        if(!owned) { continue; }
        F32* stock = th_field(db, cell, wol->stock);
        F32 extracted = *stock * take_fraction;
        *stock -= extracted;
        got += extracted * wol->yield;
      }
      th_var_set(db, this, TH_Var_FoodStore, store + got);
    }

    //- fp: Consume -- reads: population, food store; writes: population, food store
    // The phase visits every thing, but a thing with no people eats nothing.
    // The people eat first, and the food that is left over spoils in part. A
    // group that ate its fill grows by one. A group that ate less becomes
    // smaller by one.
    for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
      F32 population = th_var_get(db, this, TH_Var_Population);
      if(population <= 0) { continue; }
      F32 food = th_var_get(db, this, TH_Var_FoodStore);
      F32 eaten = Min(food, population);
      F32 spoiled = Max((food - eaten) * 0.01f, 0.0f);
      F32 change = (eaten >= population) ? 1.0f : -1.0f;
      th_var_set(db, this, TH_Var_Population, population + change);
      th_var_set(db, this, TH_Var_FoodStore, food - eaten - spoiled);
    }

    //- fp: Move -- reads: goals, board; writes: positions, move points
    // This phase is last in the tick. The Claim phase of the next tick divides
    // the board between the positions that this phase leaves. The area of a
    // thing and the position of that thing therefore always agree inside one
    // tick.
    for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
      if(th_flag_get(db, this, TH_Flag_Placed) && th_flag_get(db, this, TH_Flag_Mobile)) {
        // One point for each tick, which is one step across plains. The store
        // of points has a limit. A thing that waits on cheap ground therefore
        // cannot buy a long move across expensive ground later.
        F32* pts = th_var(db, this, TH_Var_MovePts);
        *pts = ClampTop(*pts + 1.0f, 4.0f);

        // move to the next waypoint while the points permit it
        for(;;) {
          V2I pos = gm__xy_get(db, this);
          TH_Id waypoint = th_ref_get(db, TH_Ref_Goal, this);
          if(waypoint == 0) { break; } // placed but goalless: stands still
          V2I goal = gm__xy_get(db, waypoint);

          if(v2i_eq(pos, goal)) {
            waypoint = th_ref_get(db, TH_Ref_Next, waypoint);
            th_ref_set(db, TH_Ref_Goal, this, waypoint);
            goal = gm__xy_get(db, waypoint);
          }

          V2I next = bd_path_next_towards(game->board, pos, goal);
          if(v2i_eq(next, pos)) {
            // no path from here, so go to the waypoint after it
            th_ref_set(db, TH_Ref_Goal, this, th_ref_get(db, TH_Ref_Next, waypoint));
            break;
          }

          F32 cost = bd_step_cost(game->board, pos, next);
          if(cost <= 0 || *pts < cost) { break; } // not affordable yet
          *pts -= cost;
          gm__xy_set(db, this, next);
        }
      }
    }

    th_commit(db);
    gm__extract(game);
  }
  gm__selection_refresh(game);
  arena_release_scratch(scratch);
}

internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, GM_MapMode mode) {
  V2 min = mode.min;
  V2 max = mode.max;

  TH_Db* db = game->db;
  BD_Board* board = game->board;
  GM_MapItems out = {0};
  if(max.x < min.x || max.y < min.y) { return out; }

  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  BD_PawnArray pawns = bd_pawns_all(scratch.arena, board);
  U64 cell_count = (U64)(max.x - min.x + 1) * (U64)(max.y - min.y + 1);
  out.items = push_array(arena, GM_MapItem, cell_count + pawns.count + 1); // the last item is the selection mark

  //- fp: the ground: one item at each cell of the window, with the ring of
  //  cells outside the board
  for(I32 y = min.y; y <= max.y; y += 1) {
    for(I32 x = min.x; x <= max.x; x += 1) {
      GM_MapItem* item = &out.items[out.count];
      out.count += 1;
      item->pos = (V2I){x, y};
      item->has_terrain = bd_in_bounds(board, item->pos);

      if(mode.flags & GM_MapModeFlag_Influence) {
        TH_Id home = th_field_ref_get(db, TH_FieldRef_Home, item->pos);
        item->color = col_rgb_from_hash(home);
      }

      for(I32 dy = -1; dy <= 1; dy += 1) {
        for(I32 dx = -1; dx <= 1; dx += 1) {
          item->neighbours[(dy + 1) * 3 + (dx + 1)] =
              bd_tile_at(board, v2i_add(item->pos, (V2I){dx, dy}))->terrain;
        }
      }
      BD_Tile* tile = bd_tile_at(board, item->pos);
      for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
        item->features[feature] = tile->features[feature];
      }
    }
  }

  //- fp: the surface: the pawns that stand inside the window, after the ground
  U64 num_pawns_to_show = mode.flags & GM_MapModeFlag_Pawns ? pawns.count : 0;
  for(U64 idx = 0; idx < num_pawns_to_show; idx++) {
    BD_Pawn* pawn = pawns.elems[idx];
    if(pawn->pos.x < min.x || pawn->pos.x > max.x ||
       pawn->pos.y < min.y || pawn->pos.y > max.y) { continue; }
    GM_MapItem* item = &out.items[out.count];
    out.count += 1;
    TH_Id this = (TH_Id)pawn->key;
    item->id = this;
    item->pos = pawn->pos;
    item->has_pawn = true;
    item->color = v4_splat(1);
    if(th_flag_get(db, this, TH_Flag_Debug)) {
      item->color = (V4){1, 0, 0, 1};
    }
    item->sprite = Clamp(0, th_ivar_get(db, this, TH_IVar_Sprite), GM_Sprite_COUNT - 1);
  }

  //- fp: the mark of the selection, above everything
  if(game->selection.kind != GM_SelectionKind_Nil &&
     game->selection.tile.x >= min.x && game->selection.tile.x <= max.x &&
     game->selection.tile.y >= min.y && game->selection.tile.y <= max.y) {
    GM_MapItem* item = &out.items[out.count];
    out.count += 1;
    item->pos = game->selection.tile;
    item->has_highlight = true;
  }

  arena_release_scratch(scratch);
  return out;
}


#include "game/board.c"
#include "game/worldgen.c"
#include "game/thing_db.c"
