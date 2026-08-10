////////////////////////////////
//~ fp: Game Layer -- the code of the layers below, in the order of their use

#include "game/game.h"
#include "base/arena.h"
#include "base/core.h"
#include "base/math.h"
#include "base/print.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/board.h"
#include "game/thing_db.h"
#include "game/worldgen.h"
#include "gfx/color.h"
#include "tabula.h"

// The whole part of a value, toward the lower number.
//
// Each quantity of the economy is a whole number: a group draws whole food,
// holds whole food in its granary, and holds whole people. The arithmetic uses
// F32, because a yield and a share are fractions. This function makes each
// result of that arithmetic a whole number again.
internal F32 gm__floor(F32 value) {
  F32 result = (F32)(I64)value;
  if(result > value) { result -= 1.0f; } // a cast of a value below 0 goes up
  return result;
}

// A value that is not a quantity, at two decimals for the display. A quantity
// is already a whole number and needs no such step. This function is for a
// ratio, such as the part of a tile that one group of several takes.
internal F32 gm__display_ratio(F32 value) {
  F32 offset = (value < 0) ? -0.5f : 0.5f;
  F32 result = (F32)(I32)(value * 100.0f + offset) / 100.0f;
  if(result > -0.005f && result < 0.005f) { result = 0; }
  return result;
}

////////////////////////////////
//~ fp: Group Types
//
// A group type is a way of life. It holds what a group is called and how it
// looks, and the numbers that decide what the land gives that group and how
// many people can live there.
//
// The table is private to this file. No layer above the game reads a group
// type: the display gets the numbers of a group through gm_info, as a tabula.
//
// Row 0 is the nil type (ZII). A group with row 0 draws nothing, and each
// phase of the economy passes over it.
//
// gm__group_types_load fills the whole table from the rows of a tabula file,
// in the order of the file. A second load therefore gives the same table. Call
// it after wg_terrain_table_load, because a feeding rule names a terrain and a
// terrain flag of that table.

#define GM_GROUP_TYPE_CAP 16
#define GM_FEED_RULE_CAP  16

// One rule of the feeding table of a type. A rule matches a tile when the
// terrain of that tile is the terrain of the rule, where the rule names one,
// and when that terrain holds each flag of the rule. A rule that names no
// terrain and no flag matches each tile, which gives the type a yield on any
// ground.
//
// The yield of each rule that matches goes into the sum. The traits of a tile
// therefore add together: land that is plains and that is also fertile takes
// the yield of both rules. The best ground is therefore much better than the
// average ground.
typedef struct {
  U32 terrain;               // the terrain id, or 0 for any terrain
  WG_TerrainFlags flags_all; // each flag that the terrain must hold
  F32 yield;                 // the food that this rule adds, for one tile, in one tick
} GM_FeedRule;

typedef struct {
  String8 name;       // the name to show, on the arena of the load
  GM_Sprite sprite;   // how a group of this type looks
  F32 range;          // the radius of the disc that the group draws from
  F32 population_cap; // the most people that can live here
  // The part of the food of each tick that goes to the leadership before the
  // people eat. A larger share fills the granary in fewer ticks, and leaves
  // less food for the people in each tick.
  F32 storage_share;
  F32 storage_cap; // the food that the granary holds
  GM_FeedRule rules[GM_FEED_RULE_CAP];
  U32 rule_count;
} GM_GroupType;

global GM_GroupType GM_GROUP_TYPES[GM_GROUP_TYPE_CAP];
global U32 GM_GROUP_TYPE_COUNT;

internal const GM_GroupType* gm__group_type_get(U32 idx) {
  Assert(idx < GM_GROUP_TYPE_COUNT);
  return &GM_GROUP_TYPES[idx];
}

// Row 0 is the nil type, so the search starts at row 1 and an unknown name
// gives row 0.
internal U32 gm__group_type_by_name(String8 name) {
  U32 result = 0;
  for(U32 i = 1; i < GM_GROUP_TYPE_COUNT; i += 1) {
    if(str8_match(GM_GROUP_TYPES[i].name, name, 0)) {
      result = i;
      break;
    }
  }
  return result;
}

// One `feeds` entry of a type. A key that is absent leaves its part of the
// rule empty, and an empty part matches each tile.
internal void gm__feed_rule_load(String8 path, String8 type_name, GM_GroupType* type, TB_Value* src) {
  if(type->rule_count >= GM_FEED_RULE_CAP) {
    eprintf_str8("%S: group '%S': more than %d feeds entries; extras ignored\n",
                 path, type_name, GM_FEED_RULE_CAP);
    return;
  }
  GM_FeedRule rule = {0};
  rule.yield = tb_get_num(src, str8_lit("yield"), 0.0f);

  String8 terrain_name = tb_get_str8(src, str8_lit("terrain"), str8_lit(""));
  if(terrain_name.size > 0) {
    rule.terrain = wg_terrain_by_name(terrain_name);
    // The nil terrain is row 0, and row 0 is also "any terrain" here. A name
    // that the terrain table does not hold therefore cannot become a rule,
    // because such a rule would add its yield on each tile of the world.
    if(rule.terrain == 0) {
      eprintf_str8("%S: group '%S': unknown terrain '%S'; rule dropped\n",
                   path, type_name, terrain_name);
      return;
    }
  }

  TB_Value* flag_list = tb_get(src, str8_lit("flags"));
  if(!tb_value_is_nil(flag_list) && flag_list->kind != TB_ValueKind_List) {
    eprintf_str8("%S: group '%S': flags must be a list\n", path, type_name);
  }
  for(TB_Value* elem = flag_list->first; elem != 0; elem = elem->next) {
    String8 flag_name = tb_str8_from_value(elem, str8_lit(""));
    WG_TerrainFlags flag = wg_terrain_flag_by_name(flag_name);
    if(flag == 0) {
      eprintf_str8("%S: group '%S': unknown flag '%S'\n", path, type_name, flag_name);
    }
    rule.flags_all |= flag;
  }

  type->rules[type->rule_count] = rule;
  type->rule_count += 1;
}

// `arena` holds the name of each type, so it must live as long as the table.
// gm_init passes the arena of the game, which each new world clears.
internal void gm__group_types_load(Arena* arena, String8 path) {
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);

  MemoryZeroArray(GM_GROUP_TYPES);
  GM_GROUP_TYPE_COUNT = 1; // row 0 stays the nil type

  for(TB_Node* node = root->first_member; node != 0; node = node->next) {
    if(!str8_match(node->key, str8_lit("group_type"), 0)) { continue; }
    if(GM_GROUP_TYPE_COUNT >= GM_GROUP_TYPE_CAP) {
      eprintf_str8("%S: more than %d group_type entries; extras ignored\n",
                   path, GM_GROUP_TYPE_CAP - 1);
      break;
    }
    TB_Value* src = &node->value;
    GM_GroupType* type = &GM_GROUP_TYPES[GM_GROUP_TYPE_COUNT];
    GM_GROUP_TYPE_COUNT += 1;

    // The parsed text is on the scratch arena, which this function releases.
    // The copy keeps the name for the life of the table.
    String8 name = push_str8_copy(arena, tb_get_str8(src, str8_lit("name"), str8_lit("unnamed")));
    type->name = name;

    // The art of a group. A name that no sprite holds leaves the nil sprite,
    // which draws a flat shape.
    String8 sprite_name = tb_get_str8(src, str8_lit("sprite"), str8_lit(""));
    for(U32 i = 1; i < GM_Sprite_COUNT; i += 1) {
      if(str8_match(GM_SPRITE_NAMES[i], sprite_name, 0)) {
        type->sprite = (GM_Sprite)i;
        break;
      }
    }
    if(sprite_name.size > 0 && type->sprite == GM_Sprite_Nil) {
      eprintf_str8("%S: group '%S': unknown sprite '%S'\n", path, name, sprite_name);
    }

    // Each fallback is 0, which gives a group with no reach and no people. A
    // key that is absent, and a key with a wrong spelling, therefore give a
    // group that is clearly broken on the screen.
    // A range is a distance and a share is a fraction, so both keep their
    // decimals. A count of people and an amount of food are quantities, so
    // both become whole numbers.
    type->range = tb_get_num(src, str8_lit("range"), 0.0f);
    type->storage_share = Clamp(0.0f, tb_get_num(src, str8_lit("storage_share"), 0.0f), 1.0f);
    type->population_cap = gm__floor(tb_get_num(src, str8_lit("population_cap"), 0.0f));
    type->storage_cap = gm__floor(tb_get_num(src, str8_lit("storage_cap"), 0.0f));

    for(TB_Node* member = src->first_member; member != 0; member = member->next) {
      if(!str8_match(member->key, str8_lit("feeds"), 0)) { continue; }
      gm__feed_rule_load(path, name, type, &member->value);
    }
  }
  arena_release_scratch(scratch);
}

// The type of a group. A group with no type reads row 0, which takes nothing.
internal const GM_GroupType* gm__group_type_of(TH_Db* db, TH_Id id) {
  U32 idx = (U32)th_ivar_get(db, id, TH_IVar_GroupType);
  if(idx >= GM_GROUP_TYPE_COUNT) { idx = 0; }
  return &GM_GROUP_TYPES[idx];
}

// The food of one tile for one type, before the division between the groups
// that reach that tile. The yield of each rule that matches goes into the
// sum.
internal F32 gm__tile_yield(TH_Db* db, const GM_GroupType* type, V2I pos) {
  U32 terrain = (U32)th_ifield_get(db, pos, TH_IField_Terrain);
  if(terrain >= WG_TERRAIN_TYPE_COUNT) { return 0.0f; }
  WG_TerrainFlags flags = WG_TERRAIN_TYPES[terrain].flags;
  F32 yield = 0.0f;
  for(U32 i = 0; i < type->rule_count; i += 1) {
    const GM_FeedRule* rule = &type->rules[i];
    if(rule->terrain != 0 && rule->terrain != terrain) { continue; }
    if((flags & rule->flags_all) != rule->flags_all) { continue; }
    yield += rule->yield;
  }
  return yield;
}

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
  // After the terrain table: a feeding rule names a terrain and a flag of it.
  gm__group_types_load(arena, str8_lit("data/group_types.tabula"));
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
    String8 type;
    F32 population;
  } GroupSpawn;

  // The discs of the first two cross across many tiles, so each of those tiles
  // gives each group one half. The third has a wide reach and crosses both, so
  // two kinds of way of life draw from one place. The fourth is far from the
  // other three, and shows what land gives to one group alone.
  const GroupSpawn GROUP_SPAWNS[] = {
      {str8_lit_comp("Ashfield"), {102, 96}, str8_lit_comp("village"), 10},
      {str8_lit_comp("Two Rivers"), {106, 97}, str8_lit_comp("village"), 10},
      {str8_lit_comp("Longgrass"), {96, 104}, str8_lit_comp("herders"), 10},
      {str8_lit_comp("Redhill"), {88, 118}, str8_lit_comp("band"), 10},
  };

  for(U32 idx = 0; idx < ArrayCount(GROUP_SPAWNS); ++idx) {
    const GroupSpawn* spawn = &GROUP_SPAWNS[idx];
    U32 type_idx = gm__group_type_by_name(spawn->type);
    Assert(type_idx != 0); // an unknown name is a mistake in the spawn table
    const GM_GroupType* type = gm__group_type_get(type_idx);

    TH_Id id = th_spawn(db);
    TH_Phrase* name = th_label(db, id, TH_Label_Name);
    th_push_word(name, th_define_word(db, spawn->name));
    gm__xy_set(db, id, bd_snap_passable(game->board, spawn->pos));
    th_ivar_set(db, id, TH_IVar_GroupType, (I32)type_idx);
    th_ivar_set(db, id, TH_IVar_Sprite, type->sprite);
    th_var_set(db, id, TH_Var_Population, gm__floor(spawn->population));
    th_flag_set(db, id, TH_Flag_HasInfluence, true);
    th_flag_set(db, id, TH_Flag_Placed, true);
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

  // The groups that reach this tile, and the part of its food that each one
  // takes. A tile that two groups reach gives each of them one half.
  I32 claims = th_ifield_get(db, pos, TH_IField_Claims);
  if(claims > 0) {
    tb_add_num(arena, out, str8_lit("drawn by"), (F32)claims);
    tb_add_num(arena, out, str8_lit("share each"), gm__display_ratio(1.0f / (F32)claims));
  }
}

// The economy of one group, for the display. A thing with no group type adds
// nothing here, so the panel of a wagon holds no food and no granary.
//
// Each number of the food object is the amount of one tick:
//   from land   what the tiles of the disc gave, after each overlap divided them
//   to leaders  the share that the leadership took, before the people ate
//   upkeep      the food that the people ate, which is one for each person
//   balance     from land - to leaders - upkeep. A balance below 0 comes out
//               of the granary, and the change of the granary shows that.
internal void gm__group_facts(Arena* arena, TH_Db* db, TB_Value* out, TH_Id id) {
  U32 type_idx = (U32)th_ivar_get(db, id, TH_IVar_GroupType);
  if(type_idx == 0 || type_idx >= GM_GROUP_TYPE_COUNT) { return; }
  const GM_GroupType* type = gm__group_type_get(type_idx);

  F32 population = th_var_get(db, id, TH_Var_Population);
  F32 food_in = th_var_get(db, id, TH_Var_FoodIn);
  F32 share = th_var_get(db, id, TH_Var_FoodShare);
  F32 taken = th_var_get(db, id, TH_Var_FoodTaken);
  F32 drawn = th_var_get(db, id, TH_Var_FoodDrawn);

  tb_add_str8(arena, out, str8_lit("way of life"), type->name);
  tb_add_num(arena, out, str8_lit("people"), population);
  tb_add_num(arena, out, str8_lit("people cap"), type->population_cap);

  // Each number below is already a whole number, because every phase of the
  // economy writes a whole number.
  TB_Value* food = tb_add_object(arena, out, str8_lit("food"));
  tb_add_num(arena, food, str8_lit("from land"), food_in);
  tb_add_num(arena, food, str8_lit("to leaders"), share);
  tb_add_num(arena, food, str8_lit("upkeep"), population);
  tb_add_num(arena, food, str8_lit("balance"), food_in - share - population);

  TB_Value* granary = tb_add_object(arena, out, str8_lit("granary"));
  tb_add_num(arena, granary, str8_lit("stored"), th_var_get(db, id, TH_Var_FoodStore));
  tb_add_num(arena, granary, str8_lit("capacity"), type->storage_cap);
  tb_add_num(arena, granary, str8_lit("change"), taken - drawn);
  // The part of the share that the granary had no room for. This row appears
  // only where that part is above 0, which is the state of a full granary.
  F32 wasted = share - taken;
  if(wasted > 0) {
    tb_add_num(arena, granary, str8_lit("wasted"), wasted);
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
      gm__group_facts(arena, db, info, selection.id);
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

    // The tick, in phases. Each phase reads every thing that it concerns
    // before the next phase starts. Inside a phase the things are therefore
    // independent, and the order of the slots cannot decide a result.
    //
    // Each phase states what it reads and what it writes. A phase that reads
    // across the things what it also writes across the things would break the
    // rule above. No phase does that.

    //- fp: Project -- reads: group positions, types; writes: Claims, Home
    // Each group covers a disc of the range of its type. The phase counts, at
    // each tile, the groups that reach it. A count of 2 or more is an overlap,
    // and the Draw phase below divides the food of such a tile between those
    // groups.
    //
    // A count is a sum, so the order of the groups cannot change it. Home is
    // the nearest group that reaches the tile. Where two groups are at one
    // distance, the group of the lower slot wins. Only the display reads Home,
    // so that tie changes no result of the simulation.
    {
      V2I world_size = th_world_size(db);
      U64 cell_count = (U64)world_size.x * (U64)world_size.y;
      // A distance above each distance inside a disc, so the first group that
      // covers a tile always wins the test below.
      const I32 DIST_SQ_NONE = 0x7FFFFFFF;
      I32* best_dist_sq = push_array_no_zero(scratch.arena, I32, cell_count);
      for(U64 i = 0; i < cell_count; i += 1) { best_dist_sq[i] = DIST_SQ_NONE; }
      for(I32 y = 0; y < world_size.y; y += 1) {
        for(I32 x = 0; x < world_size.x; x += 1) {
          th_ifield_set(db, (V2I){x, y}, TH_IField_Claims, 0);
          th_field_ref_set(db, TH_FieldRef_Home, (V2I){x, y}, 0);
        }
      }
      for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
          this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
        const GM_GroupType* type = gm__group_type_of(db, this);
        if(type->range <= 0) { continue; }
        for(BD_Disc it = bd_disc(game->board, gm__xy_get(db, this), type->range);
            bd_disc_next(&it);) {
          *th_ifield(db, it.pos, TH_IField_Claims) += 1;
          U64 cell = (U64)it.pos.y * world_size.x + it.pos.x;
          if(it.dist_sq < best_dist_sq[cell]) {
            best_dist_sq[cell] = it.dist_sq;
            th_field_ref_set(db, TH_FieldRef_Home, it.pos, this);
          }
        }
      }
    }

    //- fp: Draw -- reads: Claims, terrain, types; writes: food in
    // For each tile of its disc, a group takes the yield of that tile divided
    // by the number of groups that reach it. One group on a tile therefore
    // takes the whole yield, and two groups take one half each. The sum across
    // the groups is the yield of the tile, so a group gets less food where
    // another group draws from the same tile.
    //
    // The sum keeps its decimals across the tiles, and the food of the group
    // is the whole part of that sum. A yield of a fraction therefore still
    // counts: 40 tiles of 0.3 give 12 food. The whole part comes at the end,
    // so the small yields of the poor ground add together instead of falling
    // to 0 one tile at a time.
    for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
        this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
      const GM_GroupType* type = gm__group_type_of(db, this);
      F32 food_in = 0;
      if(type->range > 0) {
        for(BD_Disc it = bd_disc(game->board, gm__xy_get(db, this), type->range);
            bd_disc_next(&it);) {
          I32 claims = th_ifield_get(db, it.pos, TH_IField_Claims);
          // This group covers the tile, so the count is 1 or more. The test
          // prevents a division by 0.
          if(claims <= 0) { continue; }
          food_in += gm__tile_yield(db, type, it.pos) / (F32)claims;
        }
      }
      th_var_set(db, this, TH_Var_FoodIn, gm__floor(food_in));
    }

    //- fp: Store -- reads: food in, granary, types; writes: granary, share, taken
    // The share of the leadership comes off the food of this tick before the
    // people eat. The share leaves the food of the people in each case. The
    // granary then holds as much of that share as it has room for, and the
    // rest of the share is lost. A group with a full granary therefore loses
    // that food, and must spend from the granary to keep the next share.
    for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
        this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
      const GM_GroupType* type = gm__group_type_of(db, this);
      F32 food_in = th_var_get(db, this, TH_Var_FoodIn);
      F32 store = th_var_get(db, this, TH_Var_FoodStore);
      F32 share = gm__floor(food_in * type->storage_share);
      F32 room = ClampBot(type->storage_cap - store, 0.0f);
      F32 taken = Min(share, room);
      th_var_set(db, this, TH_Var_FoodStore, store + taken);
      th_var_set(db, this, TH_Var_FoodShare, share);
      th_var_set(db, this, TH_Var_FoodTaken, taken);
    }

    //- fp: Feed -- reads: food in, share, population, granary; writes: granary, drawn
    // One person eats one food in one tick. The food that the share left
    // covers the people first, and the granary covers what is missing. A
    // granary at 0 covers nothing more. The population does not become smaller
    // yet, so the population of a group with too little food does not change.
    for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
        this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
      F32 available = th_var_get(db, this, TH_Var_FoodIn) - th_var_get(db, this, TH_Var_FoodShare);
      F32 demand = th_var_get(db, this, TH_Var_Population);
      F32 drawn = 0;
      if(available < demand) {
        F32 store = th_var_get(db, this, TH_Var_FoodStore);
        drawn = Min(demand - available, store);
        th_var_set(db, this, TH_Var_FoodStore, store - drawn);
      }
      th_var_set(db, this, TH_Var_FoodDrawn, drawn);
    }

    //- fp: Grow -- reads: food in, share, population, types; writes: population
    // The population grows by one person where the food that the share left
    // feeds the people of now and that one person, and where the population is
    // below the limit of the type. Food above both limits is lost, because
    // this economy holds no food outside the granary.
    //
    // The test reads population + 1 and not population, so a group grows only
    // where it can feed the person that it adds. A group at rest therefore
    // keeps one food spare in each tick, and does not stand at the point where
    // any loss becomes a shortage.
    for(TH_Id this = th_first_flagged(db, TH_Flag_HasInfluence);
        this != 0; this = th_next_flagged(db, TH_Flag_HasInfluence, this)) {
      const GM_GroupType* type = gm__group_type_of(db, this);
      F32 available = th_var_get(db, this, TH_Var_FoodIn) - th_var_get(db, this, TH_Var_FoodShare);
      F32 population = th_var_get(db, this, TH_Var_Population);
      if(available > population + 1.0f && population < type->population_cap) {
        th_var_set(db, this, TH_Var_Population, population + 1.0f);
      }
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
