////////////////////////////////
//~ fp: Game Layer -- implementations of the layers below, in dependency order

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

////////////////////////////////
//~ fp: Game

// mirror the authoritative fields into the derived board: terrain ids,
// feature masks, and travel rules (costs cached from the terrain table),
// then drop the path cache. Call after any field write.
StaticAssert(WG_TERRAIN_CAP <= BD_TERRAIN_CAP, gm_terrain_costs_fit);
internal void gm__board_mirror(GM_Game* game, WG_Params* params) {
  TH_Db* db = game->db;
  BD_Board* board = game->board;
  for(I32 y = 0; y < board->height; y += 1) {
    for(I32 x = 0; x < board->width; x += 1) {
      V2I p = {x, y};
      BD_Tile* tile = bd_tile_at(board, p);
      tile->terrain = (BD_Terrain)th_ifield_get(db, p, TH_IField_Terrain);
      tile->features[BD_Feature_River] = (U8)th_ifield_get(db, p, TH_IField_RiverMask);
      tile->features[BD_Feature_Road] = (U8)th_ifield_get(db, p, TH_IField_RoadMask);
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

// demo dressing: a road between two would-be settlements on opposite sides
// of the continent, following the terrain's own best path -- routed on the
// mirrored board, written into the fields, mirrored back
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

/////// Semantic spatial search
//
//

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
  // a negative outer radius reaches nothing, and would invert the extents below
  if(query.radius_max < 0) { return out; }

  // Calculate extents
  I32 x_min = Clamp(0, query.focus.x - query.radius_max, board->width - 1);
  I32 y_min = Clamp(0, query.focus.y - query.radius_max, board->height - 1);
  I32 x_max = Clamp(0, query.focus.x + query.radius_max, board->width - 1);
  I32 y_max = Clamp(0, query.focus.y + query.radius_max, board->height - 1);

  U32 hits_cap = (x_max - x_min + 1) * (y_max - y_min + 1);
  out.tiles = push_array_no_zero(arena, GM_TileHit, hits_cap);
  out.count = 0;

  // ring bounds, squared once so the per-tile test needs no sqrt. A negative
  // radius_min must not square back into an exclusion.
  I32 radius_max_sq = query.radius_max * query.radius_max;
  I32 radius_min_sq = query.radius_min > 0 ? query.radius_min * query.radius_min : 0;

  for(I32 y = y_min; y <= y_max; ++y) {
    for(I32 x = x_min; x <= x_max; ++x) {
      // euclidean, inclusive at both ends -- the same distance influence uses.
      // Tested before the tile is fetched: it rejects the ~21% of the bounding
      // box outside the disc, plus whatever radius_min carves out.
      I32 dx = x - query.focus.x;
      I32 dy = y - query.focus.y;
      I32 dist_sq = dx * dx + dy * dy;
      if(dist_sq > radius_max_sq || dist_sq < radius_min_sq) { continue; }

      V2I pos = {x, y};
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
  }
  return out;
}

// extraction: project db facts into the derived layers, in full, every tick.
// Today that is only the board; future extractions (indices, rosters, render
// data) hang off the same single pass over every thing, each checking the
// facts it cares about. Nothing else may write the derived layers.
internal void gm__extract(GM_Game* game) {
  TH_Db* db = game->db;

  //- board, pawn side first: no pawn goes unexamined, so anything whose
  // thing is gone or unplaced is guaranteed to be removed here
  ArenaTemp scratch = arena_get_scratch(0, 0);
  BD_PawnArray pawns = bd_pawns_all(scratch.arena, game->board);
  for(U64 i = 0; i < pawns.count; i += 1) {
    // a stale id reads no flags, so dead things fail this check too
    TH_Id id = (TH_Id)pawns.elems[i]->key;
    if(!th_flag_get(db, id, TH_Flag_Placed)) {
      bd_pawn_remove(game->board, id);
    }
  }
  arena_release_scratch(scratch);

  //- thing side: the one loop every extraction shares. Placed things upsert
  // their pawn here -- creation is the job no pawn-side sweep can do, since
  // a pawn that does not exist yet is never in the sweep
  for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
    if(th_flag_get(db, this, TH_Flag_Placed)) {
      bd_pawn_place(game->board, this, gm__xy_get(db, this));
    }
  }
}

// home: which thing each tile belongs to. Every thing flagged HasInfluence
// contests the board, and the winner of a tile becomes its Home -- a
// contested or unreached tile belongs to nobody (nil). One claim is like
// every other for now: the rules below are the same for all sources, so
// distance alone settles a border. Unlike extraction this writes the db, so
// it runs after it, on the pawn positions extraction just reconciled.
#define GM_INFLUENCE_STRENGTH 1.0f
#define GM_INFLUENCE_RANGE    4.0f
#define GM_INFLUENCE_DECAY    BD_InfluenceDecay_Linear
internal void gm__home_derive(GM_Game* game) {
  TH_Db* db = game->db;
  BD_Board* board = game->board;
  ArenaTemp scratch = arena_get_scratch(0, 0);

  // count, then fill -- two cheap passes beat growable storage. Sources that
  // are not on the board find no pawn and simply claim nothing.
  BD_InfluenceArray sources = {0};
  for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
      this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
    sources.count += 1;
  }
  sources.elems = push_array_no_zero(scratch.arena, BD_Influence, sources.count);
  U64 at = 0;
  for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
      this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
    sources.elems[at] = (BD_Influence){
        .key = this, // a placed thing is its own pawn's key
        .strength = GM_INFLUENCE_STRENGTH,
        .range = GM_INFLUENCE_RANGE,
        .decay = GM_INFLUENCE_DECAY,
    };
    at += 1;
  }

  // nil is the unassigned key: th_field_ref_set reads it as "clear this tile"
  BD_InfluenceAssignment* map = bd_influence_map(scratch.arena, board, sources, 0);
  for(I32 y = 0; y < board->height; y += 1) {
    for(I32 x = 0; x < board->width; x += 1) {
      TH_Id home = (TH_Id)map[(U64)y * board->width + x].key;
      th_field_ref_set(db, TH_FieldRef_Home, (V2I){x, y}, home);
    }
  }
  arena_release_scratch(scratch);
}

internal void gm_init(Arena* arena, GM_Game* game, U64 seed) {
  MemoryZeroStruct(game);
  game->db = th_init_db(arena);

  //- fp: the world: terrain registry and knobs from the file, generated
  //  into the fields, then mirrored into the board
  wg_terrain_table_load(str8_lit("data/terrain_types.tabula"));
  WG_Params params = wg_params_load(str8_lit("data/world.tabula"));
  wg_generate(game->db, &params, seed);
  game->board = bd_board_alloc(arena, params.width, params.height, 1024);
  gm__board_mirror(game, &params);
  gm__demo_road(game, &params);

  TH_Db* db = game->db;

  // corner-ish waypoints, snapped onto whatever land this world grew there,
  // chained into a patrol cycle via Next
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
    const char* name;
    V2I pos;
    GM_Sprite sprite;
    F32 population;
  } GroupSpawn;

  const GroupSpawn GROUP_SPAWNS[] = {
      {"Place #A", {100, 100}, GM_Sprite_Band, .population = 200},
      {"Place #B", {102, 96}, GM_Sprite_Tholos, .population = 500},
  };

  for(U32 idx = 0; idx < ArrayCount(GROUP_SPAWNS); ++idx) {
    const GroupSpawn* spawn = &GROUP_SPAWNS[idx];
    TH_Id id = th_spawn(db);
    TH_Phrase* name = th_label(db, id, TH_Label_Name);
    th_push_word(name, th_define_word(db, str8_cstring(spawn->name)));
    gm__xy_set(db, id, spawn->pos);
    th_var_set(db, id, TH_Var_Population, spawn->population);
    th_ivar_set(db, id, TH_IVar_Sprite, spawn->sprite);
    th_flag_set(db, id, TH_Flag_HasInfluence, true);
    th_flag_set(db, id, TH_Flag_Placed, true);
  }

  th_commit(db);
  gm__extract(game);
  gm__home_derive(game);
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

// re-derive a thing selection's tile from the thing; a dead or unplaced
// thing clears the selection. Runs every update.
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
  if(tile->features[BD_Feature_River] != 0 || tile->features[BD_Feature_Road] != 0) {
    TB_Value* features = tb_add_list(arena, out, str8_lit("features"));
    if(tile->features[BD_Feature_River] != 0) { tb_list_push_str8(arena, features, str8_lit("river")); }
    if(tile->features[BD_Feature_Road] != 0) { tb_list_push_str8(arena, features, str8_lit("road")); }
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
    } break;
    case GM_SelectionKind_Thing: {
      tb_add_str8(arena, info, str8_lit("kind"), str8_lit("thing"));
      GM_Sprite sprite = (GM_Sprite)Clamp(0, th_ivar_get(db, selection.id, TH_IVar_Sprite), GM_Sprite_COUNT - 1);
      String8 name = gm__thing_name(arena, db, selection.id);
      if(name.size == 0) { name = GM_SPRITE_NAMES[sprite]; }
      tb_add_str8(arena, info, str8_lit("name"), name);
      tb_add_str8(arena, info, str8_lit("sprite"), GM_SPRITE_NAMES[sprite]);
      tb_add_num(arena, info, str8_lit("population"), th_var_get(db, selection.id, TH_Var_Population));
    } break;
  }
  gm__tile_facts(arena, game, tb_add_object(arena, info, str8_lit("tile")), selection.tile);
}

internal TB_Value* gm_info(Arena* arena, GM_Game* game) {
  TB_Value* info = tb_build_object(arena);
  gm__selection_info(arena, game, info);
  return info;
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

    // DO NOT REWRITE THIS AS A LOOP OVER SOME FLAGGED ENTITY.
    // WE WANT TO SWEEP EVERY ENTITY HERE
    for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
      if(th_flag_get(db, this, TH_Flag_Placed) && th_flag_get(db, this, TH_Flag_Mobile)) {
        // 1 point per tick: a plains step; banking is capped so waiting at a
        // cheap stretch cannot buy a later teleport across an expensive one
        F32* pts = th_var(db, this, TH_Var_MovePts);
        *pts = ClampTop(*pts + 1.0f, 4.0f);

        // Advance waypoints while we can
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
            // unreachable from here: skip that waypoint
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
    gm__home_derive(game);
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
  out.items = push_array(arena, GM_MapItem, cell_count + pawns.count + 1); // +1: selection highlight

  //- fp: ground segment: one item per window cell, off-board ring included
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

  //- fp: surface segment: pawns standing inside the window, after the ground
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

  //- fp: highlight segment: the selection marker, above everything
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
