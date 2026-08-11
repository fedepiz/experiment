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

// Each quantity of the economy is whole, and the arithmetic that makes it is
// F32.
internal F32 gm__floor(F32 value) {
  F32 result = (F32)(I64)value;
  if(result > value) { result -= 1.0f; } // a cast of a value below 0 goes up
  return result;
}

// For a ratio, which is the one number of the display that is not whole.
internal F32 gm__display_ratio(F32 value) {
  F32 offset = (value < 0) ? -0.5f : 0.5f;
  F32 result = (F32)(I32)(value * 100.0f + offset) / 100.0f;
  if(result > -0.005f && result < 0.005f) { result = 0; }
  return result;
}

////////////////////////////////
//~ fp: Group Types
//
// The table is private to this file. No layer above the game reads a group
// type: the display gets the numbers of a group through gm_info, as a tabula.
//
// Row 0 is the nil type (ZII). A group with row 0 draws nothing.
//
// Load after wg_terrain_table_load: a feeding rule names a terrain of it.

#define GM_GROUP_TYPE_CAP 16
#define GM_FEED_RULE_CAP  16

// A rule with no terrain and no flag matches each tile. Every rule that
// matches adds its yield, so the traits of a tile add together.
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
  F32 storage_share;  // the part of each tick's food that goes to the granary first
  F32 storage_cap;    // the food that the granary holds
  GM_FeedRule rules[GM_FEED_RULE_CAP];
  U32 rule_count;
} GM_GroupType;

global GM_GroupType GM_GROUP_TYPES[GM_GROUP_TYPE_CAP];
global U32 GM_GROUP_TYPE_COUNT;

// An index outside the table reads row 0, which is the nil type.
internal const GM_GroupType* gm__group_type_get(U32 idx) {
  if(idx >= GM_GROUP_TYPE_COUNT) { idx = 0; }
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

// The file and the group row that the loader reads now. It exists for the
// messages, so a load function does not carry two strings for its prefix. An
// empty `group` drops that part of the prefix.
typedef struct {
  String8 path;
  String8 group;
} GM_GroupTypeSource;

internal void gm__group_type_error(GM_GroupTypeSource source, char* fmt, ...) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 msg = push_str8fv(scratch.arena, fmt, args);
  va_end(args);
  if(source.group.size > 0) {
    eprintf_str8("%S: group '%S': %S\n", source.path, source.group, msg);
  } else {
    eprintf_str8("%S: %S\n", source.path, msg);
  }
  arena_release_scratch(scratch);
}

// The row of GM_SPRITE_NAMES that holds `name`. Row 0 is the nil sprite, so
// the search starts at 1 and an unknown name gives 0.
internal U32 gm__sprite_by_name(String8 name) {
  U32 result = 0;
  for(U32 i = 1; i < GM_Sprite_COUNT; i += 1) {
    if(str8_match(GM_SPRITE_NAMES[i], name, 0)) {
      result = i;
      break;
    }
  }
  return result;
}

// One `feeds` entry of a type. A key that is absent leaves its part of the
// rule empty, and an empty part matches each tile.
internal void gm__feed_rule_load(GM_GroupTypeSource source, GM_GroupType* type, TB_Value* entry) {
  if(type->rule_count >= GM_FEED_RULE_CAP) {
    gm__group_type_error(source, "more than %d feeds entries; extras ignored", GM_FEED_RULE_CAP);
    return;
  }
  GM_FeedRule rule = {0};
  rule.yield = tb_get_num(entry, str8_lit("yield"), 0.0f);

  String8 terrain_name = tb_get_str8(entry, str8_lit("terrain"), str8_lit(""));
  if(terrain_name.size > 0) {
    rule.terrain = wg_terrain_by_name(terrain_name);
    // The nil terrain is row 0, and row 0 is also "any terrain" here. A name
    // that the terrain table does not hold therefore cannot become a rule,
    // because such a rule would add its yield on each tile of the world.
    if(rule.terrain == 0) {
      gm__group_type_error(source, "unknown terrain '%S'; rule dropped", terrain_name);
      return;
    }
  }

  TB_Value* flag_list = tb_get(entry, str8_lit("flags"));
  if(!tb_value_is_nil(flag_list) && flag_list->kind != TB_ValueKind_List) {
    gm__group_type_error(source, "flags must be a list");
  }
  for(TB_Value* elem = flag_list->first; elem != 0; elem = elem->next) {
    String8 flag_name = tb_str8_from_value(elem, str8_lit(""));
    WG_TerrainFlags flag = wg_terrain_flag_by_name(flag_name);
    if(flag == 0) {
      gm__group_type_error(source, "unknown flag '%S'", flag_name);
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
  GM_GroupTypeSource source = {path, str8_lit("")};

  MemoryZeroArray(GM_GROUP_TYPES);
  GM_GROUP_TYPE_COUNT = 1; // row 0 stays the nil type

  for(TB_Node* node = root->first_member; node != 0; node = node->next) {
    if(!str8_match(node->key, str8_lit("group_type"), 0)) { continue; }
    if(GM_GROUP_TYPE_COUNT >= GM_GROUP_TYPE_CAP) {
      gm__group_type_error(source, "more than %d group_type entries; extras ignored",
                           GM_GROUP_TYPE_CAP - 1);
      break;
    }
    TB_Value* body = &node->value;
    GM_GroupType* type = &GM_GROUP_TYPES[GM_GROUP_TYPE_COUNT];
    GM_GROUP_TYPE_COUNT += 1;

    // The parsed text is on the scratch arena, which this function releases.
    // The copy keeps the name for the life of the table.
    String8 name = push_str8_copy(arena, tb_get_str8(body, str8_lit("name"), str8_lit("unnamed")));
    type->name = name;
    source.group = name;

    // The art of a group. A name that no sprite holds leaves the nil sprite,
    // which draws a flat shape.
    String8 sprite_name = tb_get_str8(body, str8_lit("sprite"), str8_lit(""));
    type->sprite = (GM_Sprite)gm__sprite_by_name(sprite_name);
    if(sprite_name.size > 0 && type->sprite == GM_Sprite_Nil) {
      gm__group_type_error(source, "unknown sprite '%S'", sprite_name);
    }

    // Each fallback is 0, which gives a group with no reach and no people. A
    // key that is absent, and a key with a wrong spelling, therefore give a
    // group that is clearly broken on the screen.
    // A range is a distance and a share is a fraction, so both keep their
    // decimals. A count of people and an amount of food are quantities, so
    // both become whole numbers.
    type->range = tb_get_num(body, str8_lit("range"), 0.0f);
    type->storage_share = Clamp(0.0f, tb_get_num(body, str8_lit("storage_share"), 0.0f), 1.0f);
    type->population_cap = gm__floor(tb_get_num(body, str8_lit("population_cap"), 0.0f));
    type->storage_cap = gm__floor(tb_get_num(body, str8_lit("storage_cap"), 0.0f));

    for(TB_Node* member = body->first_member; member != 0; member = member->next) {
      if(!str8_match(member->key, str8_lit("feeds"), 0)) { continue; }
      gm__feed_rule_load(source, type, &member->value);
    }
    source.group = str8_lit("");
  }
  arena_release_scratch(scratch);
}

// The type of a group. A group with no type reads row 0, which takes nothing.
internal const GM_GroupType* gm__group_type_of(TH_Db* db, TH_Id id) {
  return gm__group_type_get((U32)th_ivar_get(db, id, TH_IVar_GroupType));
}

// Before the division between the groups that reach that tile.
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

internal void gm_init(GM_Game* game, U64 seed) {
  // The arena survives the zeroing below: the game owns it for the run of the
  // program, and each new world clears it.
  Arena* arena = game->arena;
  if(arena == 0) { arena = arena_alloc(); }
  arena_clear(arena);
  MemoryZeroStruct(game);
  game->arena = arena;

  game->db = th_init_db(arena);

  //- fp: the world. Read the parameters from the files, generate the world
  //  into the fields, then copy the fields into the board. The caller has
  //  loaded the terrain table already: a feeding rule names a terrain and a
  //  flag of it.
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
    // An unknown name gives row 0: a group with no reach and no people, which
    // is clearly broken on the screen.
    U32 type_idx = gm__group_type_by_name(spawn->type);
    if(type_idx == 0) {
      eprintf_str8("spawn '%S': unknown group type '%S'\n", spawn->name, spawn->type);
    }
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

// The forms of `type` are in hud__typed_row.
internal TB_Value* gm__fact_row(Arena* arena, TB_Value* out, String8 key, String8 name, String8 type) {
  TB_Value* row = tb_add_object(arena, out, key);
  tb_add_str8(arena, row, str8_lit("name"), name);
  tb_add_str8(arena, row, str8_lit("type"), type);
  return row;
}

internal void gm__tile_facts(Arena* arena, GM_Game* game, TB_Value* out, V2I pos) {
  TH_Db* db = game->db;
  TB_Value* xy = gm__fact_row(arena, out, str8_lit("pos"), str8_lit("Position"), str8_lit("text"));
  tb_add_str8(arena, xy, str8_lit("value"), push_str8f(arena, "%d, %d", pos.x, pos.y));

  U32 terrain = (U32)th_ifield_get(db, pos, TH_IField_Terrain);
  TB_Value* terrain_row = gm__fact_row(arena, out, str8_lit("terrain"), str8_lit("Terrain"), str8_lit("text"));
  tb_add_str8(arena, terrain_row, str8_lit("value"), wg_terrain_name(terrain));
  if(terrain < WG_TERRAIN_TYPE_COUNT) {
    TB_Value* cost = gm__fact_row(arena, out, str8_lit("move_cost"), str8_lit("Move Cost"), str8_lit("number"));
    tb_add_num(arena, cost, str8_lit("value"), WG_TERRAIN_TYPES[terrain].move_cost);
  }

  BD_Tile* tile = bd_tile_at(game->board, pos);
  String8List features = {0};
  for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
    if(tile->features[feature] == 0) { continue; }
    str8_list_push(arena, &features, GM_FEATURE_NAMES[feature]);
  }
  // A tile with no feature carries no features row.
  if(features.node_count > 0) {
    StringJoin join = {str8_lit(""), str8_lit(", "), str8_lit("")};
    TB_Value* row = gm__fact_row(arena, out, str8_lit("features"), str8_lit("Features"), str8_lit("text"));
    tb_add_str8(arena, row, str8_lit("value"), str8_list_join(arena, features, &join));
  }

  TH_Id home = th_field_ref_get(db, TH_FieldRef_Home, pos);
  if(home != 0) {
    String8 name = gm__thing_name(arena, db, home);
    if(name.size > 0) {
      TB_Value* row = gm__fact_row(arena, out, str8_lit("home"), str8_lit("Home"), str8_lit("text"));
      tb_add_str8(arena, row, str8_lit("value"), name);
    }
  }

  // The groups that reach this tile, and the part of its food that each one
  // takes. A tile that two groups reach gives each of them one half.
  I32 claims = th_ifield_get(db, pos, TH_IField_Claims);
  if(claims > 0) {
    TB_Value* drawn = gm__fact_row(arena, out, str8_lit("drawn_by"), str8_lit("Drawn By"), str8_lit("number"));
    tb_add_num(arena, drawn, str8_lit("value"), (F32)claims);
    TB_Value* share = gm__fact_row(arena, out, str8_lit("share_each"), str8_lit("Share Each"), str8_lit("number"));
    tb_add_num(arena, share, str8_lit("value"), gm__display_ratio(1.0f / (F32)claims));
  }
}

// A thing with no group type adds nothing, so a wagon holds no food row.
internal void gm__group_facts(Arena* arena, TH_Db* db, TB_Value* out, TH_Id id) {
  U32 type_idx = (U32)th_ivar_get(db, id, TH_IVar_GroupType);
  if(type_idx == 0 || type_idx >= GM_GROUP_TYPE_COUNT) { return; }
  const GM_GroupType* type = gm__group_type_get(type_idx);

  F32 population = th_var_get(db, id, TH_Var_Population);
  F32 food_in = th_var_get(db, id, TH_Var_FoodIn);
  F32 share = th_var_get(db, id, TH_Var_FoodShare);
  F32 taken = th_var_get(db, id, TH_Var_FoodTaken);
  F32 drawn = th_var_get(db, id, TH_Var_FoodDrawn);

  TB_Value* way = gm__fact_row(arena, out, str8_lit("way_of_life"), str8_lit("Way of Life"), str8_lit("text"));
  tb_add_str8(arena, way, str8_lit("value"), type->name);

  TB_Value* people = gm__fact_row(arena, out, str8_lit("people"), str8_lit("People"), str8_lit("x_of_y"));
  tb_add_num(arena, people, str8_lit("value"), population);
  tb_add_num(arena, people, str8_lit("limit"), type->population_cap);

  TB_Value* food = gm__fact_row(arena, out, str8_lit("food"), str8_lit("Food"), str8_lit("balance"));
  tb_add_num(arena, food, str8_lit("value"), food_in - share - population);
  tb_add_str8(arena, food, str8_lit("hover"),
              push_str8f(arena, "land %g - granary %g - upkeep %g", food_in, share, population));

  TB_Value* granary = gm__fact_row(arena, out, str8_lit("granary"), str8_lit("Granary"), str8_lit("x_of_y"));
  tb_add_num(arena, granary, str8_lit("value"), th_var_get(db, id, TH_Var_FoodStore));
  tb_add_num(arena, granary, str8_lit("limit"), type->storage_cap);
  // The change of this tick, and the part of the share that the granary had no
  // room for. That part is above 0 only where the granary is full.
  F32 change = taken - drawn;
  F32 wasted = share - taken;
  String8 hover = (change == 0) ? str8_lit("no change this tick")
                                : push_str8f(arena, "%+g this tick", change);
  if(wasted > 0) {
    hover = push_str8f(arena, "%S, %g wasted: the granary is full", hover, wasted);
  }
  tb_add_str8(arena, granary, str8_lit("hover"), hover);
}

internal void gm__selection_info(Arena* arena, GM_Game* game, TB_Value* root) {
  TH_Db* db = game->db;
  GM_Selection selection = game->selection;
  if(selection.kind == GM_SelectionKind_Nil) { return; }
  TB_Value* info = tb_add_object(arena, root, str8_lit("selection"));
  if(selection.kind == GM_SelectionKind_Thing) {
    GM_Sprite sprite = (GM_Sprite)Clamp(0, th_ivar_get(db, selection.id, TH_IVar_Sprite), GM_Sprite_COUNT - 1);
    String8 name = gm__thing_name(arena, db, selection.id);
    if(name.size == 0) { name = GM_SPRITE_NAMES[sprite]; }
    tb_add_str8(arena, info, str8_lit("name"), name);
    gm__group_facts(arena, db, info, selection.id);
  }
  TB_Value* tile = tb_add_object(arena, info, str8_lit("tile"));
  tb_add_str8(arena, tile, str8_lit("name"), str8_lit("Tile"));
  gm__tile_facts(arena, game, tile, selection.tile);
}

internal TB_Value* gm_info(Arena* arena, GM_Game* game) {
  TB_Value* info = tb_build_object(arena);
  gm__selection_info(arena, game, info);
  return info;
}

////////////////////////////////
//~ fp: Economy

// The groups of one tick, as a list on the scratch of that tick. The Project
// phase fills `hits` with the tiles of the disc of the group, and the Economy
// phase reads those tiles again, so one walk of the disc serves both phases.
typedef struct GM_GroupNode GM_GroupNode;
struct GM_GroupNode {
  GM_GroupNode* next;
  TH_Id id;
  const GM_GroupType* type;
  GM_TileHit* hits; // one contiguous block, sized by bd_disc_bound
  U64 hit_count;
};

typedef struct {
  GM_GroupNode* first;
  GM_GroupNode* last;
  U64 count;
} GM_GroupList;

// Each read and each write is of this group, so the tick can run the groups
// in any order.
internal void gm__group_economy(GM_Game* game, GM_GroupNode* node) {
  TH_Db* db = game->db;
  TH_Id id = node->id;
  const GM_GroupType* type = node->type;

  // The sum keeps its decimals to the end, so many small yields still come to
  // food.
  F32 food_in = 0;
  for(U64 i = 0; i < node->hit_count; i += 1) {
    V2I pos = node->hits[i].coords;
    I32 claims = th_ifield_get(db, pos, TH_IField_Claims);
    if(claims <= 0) { continue; } // this group covers the tile, so the count is 1 or more
    food_in += gm__tile_yield(db, type, pos) / (F32)claims;
  }
  food_in = gm__floor(food_in);

  // The share leaves the people either way, room for it or not.
  F32 store = th_var_get(db, id, TH_Var_FoodStore);
  F32 share = gm__floor(food_in * type->storage_share);
  F32 taken = Min(share, ClampBot(type->storage_cap - store, 0.0f));
  store += taken;

  // One person eats one food.
  F32 population = th_var_get(db, id, TH_Var_Population);
  F32 available = food_in - share;
  F32 drawn = Min(ClampBot(population - available, 0.0f), store);
  store -= drawn;

  if(available > population + 1.0f && population < type->population_cap) {
    population += 1.0f;
  }

  // Store and population are state; the four food numbers are the record.
  th_var_set(db, id, TH_Var_FoodStore, store);
  th_var_set(db, id, TH_Var_Population, population);
  th_var_set(db, id, TH_Var_FoodIn, food_in);
  th_var_set(db, id, TH_Var_FoodShare, share);
  th_var_set(db, id, TH_Var_FoodTaken, taken);
  th_var_set(db, id, TH_Var_FoodDrawn, drawn);
}

////////////////////////////////
//~ fp: Tick

#define GM_TICK_DT       0.1f // seconds of sim per tick; every rate below is per-tick
#define GM_TICK_BANK_MAX 0.5f // at most 5 banked ticks replay after a stall

internal void gm_update(GM_Game* game, F32 dt) {
  TH_Db* db = game->db;
  dt *= game->paused ? 0 : 1;
  game->move_timer = ClampTop(game->move_timer + dt, GM_TICK_BANK_MAX);
  while(game->move_timer > GM_TICK_DT) {
    // The scratch lives for one tick, so the ticks that a stall banks do not
    // stack their allocations.
    ArenaTemp scratch = arena_get_scratch(0, 0);

    game->tick_num++;
    game->move_timer -= GM_TICK_DT;

    // No phase reads across the things what it also writes across the things,
    // so the order of the slots decides no result.

    // One general pass over the things, so other lists can join it later.
    GM_GroupList groups = {0};
    for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
      if(th_flag_get(db, this, TH_Flag_HasInfluence)) {
        GM_GroupNode* node = push_array(scratch.arena, GM_GroupNode, 1);
        node->id = this;
        node->type = gm__group_type_of(db, this);
        SLLQueuePush(groups.first, groups.last, node);
        groups.count += 1;
      }
    }

    //- fp: Project -- reads: group positions, types; writes: Claims, Home
    // Claims is a sum, so the order of the groups cannot change it. Home ties
    // to the lower slot, and only the display reads Home.
    {
      V2I world_size = th_world_size(db);
      U64 cell_count = (U64)world_size.x * (U64)world_size.y;
      // A distance above each distance inside a disc, so the first group that
      // covers a tile always wins the test below.
      const I32 DIST_SQ_NONE = 0x7FFFFFFF;
      I32* best_dist_sq = push_array_no_zero(scratch.arena, I32, cell_count);
      for(U64 i = 0; i < cell_count; i += 1) { best_dist_sq[i] = DIST_SQ_NONE; }
      th_ifield_clear(db, TH_IField_Claims);
      th_field_ref_clear(db, TH_FieldRef_Home);

      for(GM_GroupNode* node = groups.first; node != 0; node = node->next) {
        if(node->type->range <= 0) { continue; }
        BD_Disc disc = bd_disc(game->board, gm__xy_get(db, node->id), node->type->range);
        node->hits = push_array_no_zero(scratch.arena, GM_TileHit, bd_disc_bound(disc));
        while(bd_disc_next(&disc)) {
          *th_ifield(db, disc.pos, TH_IField_Claims) += 1;
          U64 cell = (U64)disc.pos.y * world_size.x + disc.pos.x;
          if(disc.dist_sq < best_dist_sq[cell]) {
            best_dist_sq[cell] = disc.dist_sq;
            th_field_ref_set(db, TH_FieldRef_Home, disc.pos, node->id);
          }
          node->hits[node->hit_count] = (GM_TileHit){disc.pos};
          node->hit_count += 1;
        }
      }
    }

    //- fp: Economy -- reads: Claims, terrain, types, group facts; writes: group facts
    // The rules are in gm__group_economy. A group reads and writes its own
    // facts only, so the whole economy is one pass in any order.
    for(GM_GroupNode* node = groups.first; node != 0; node = node->next) {
      gm__group_economy(game, node);
    }

    //- fp: Move -- reads: goals, board; writes: positions, move points
    // Last in the tick, so the area of a thing and the position of that thing
    // always agree inside one tick.
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
    arena_release_scratch(scratch);
  }
  gm__selection_refresh(game);
}

// How much of the ground below a claim a person sees. A shape carries the
// color to draw, so the strength of a claim is here and in no other place.
// Two claims on one tile add together, and that tile becomes darker.
#define GM_CLAIM_ALPHA 0.35f

// The next free shape of the list, zeroed. A full last chunk starts a new one.
internal GM_MapShape* gm__map_shape_push(Arena* arena, GM_MapShapeList* list) {
  GM_MapShapeChunk* chunk = list->last;
  if(chunk == 0 || chunk->count >= GM_MAP_SHAPE_CHUNK_CAP) {
    chunk = push_array(arena, GM_MapShapeChunk, 1);
    SLLQueuePush(list->first, list->last, chunk);
  }
  list->total_count += 1;
  return &chunk->shapes[chunk->count++];
}

internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, GM_MapMode mode) {
  Rng2I32 window = mode.window;

  TH_Db* db = game->db;
  BD_Board* board = game->board;
  GM_MapItems out = {0};
  if(rng2i32_is_empty(window)) { return out; }

  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  BD_PawnArray pawns = bd_pawns_all(scratch.arena, board);

  //- fp: the ground: one cell at each tile of the window, with the ring of
  //  tiles outside the board
  U64 ground_cap = rng2i32_area(window);
  out.ground = push_array(arena, GM_MapGround, ground_cap);
  for(I32 y = window.min.y; y <= window.max.y; y += 1) {
    for(I32 x = window.min.x; x <= window.max.x; x += 1) {
      Assert(out.ground_count < ground_cap);
      GM_MapGround* cell = &out.ground[out.ground_count++];
      cell->pos = (V2I){x, y};
      for(I32 dy = -1; dy <= 1; dy += 1) {
        for(I32 dx = -1; dx <= 1; dx += 1) {
          cell->neighbours[(dy + 1) * 3 + (dx + 1)] =
              bd_tile_at(board, v2i_add(cell->pos, (V2I){dx, dy}))->terrain;
        }
      }
      BD_Tile* tile = bd_tile_at(board, cell->pos);
      for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
        cell->features[feature] = tile->features[feature];
      }
    }
  }

  //- fp: the shapes above the ground, in the order of their draw. A claim goes
  //  down first, so a thing that stands on a tile covers the claims of it.

  // One shape for each claim that a group holds on a tile of the window. Two
  // groups that reach one tile give that tile two shapes, so the count of the
  // shapes is the count of the claims and not of the tiles. The claims come
  // from the database at this call, so they always agree with the last tick.
  if(mode.flags & GM_MapModeFlag_Influence) {
    for(TH_Id this = th_first(db); this != 0; this = th_next(db, this)) {
      if(!th_flag_get(db, this, TH_Flag_HasInfluence)) { continue; }
      const GM_GroupType* type = gm__group_type_of(db, this);
      if(type->range <= 0) { continue; }
      for(BD_Disc it = bd_disc(board, gm__xy_get(db, this), type->range);
          bd_disc_next(&it);) {
        if(!rng2i32_contains(window, it.pos)) { continue; }
        GM_MapShape* shape = gm__map_shape_push(arena, &out.shapes);
        shape->pos = it.pos;
        shape->color = col_rgb_from_hash(this);
        shape->color.w = GM_CLAIM_ALPHA;
      }
    }
  }

  // One shape for each pawn that stands inside the window.
  if(mode.flags & GM_MapModeFlag_Pawns) {
    for(U64 idx = 0; idx < pawns.count; idx += 1) {
      BD_Pawn* pawn = pawns.elems[idx];
      if(!rng2i32_contains(window, pawn->pos)) { continue; }
      GM_MapShape* shape = gm__map_shape_push(arena, &out.shapes);
      TH_Id this = (TH_Id)pawn->key;
      shape->id = this;
      shape->pos = pawn->pos;
      shape->color = v4_splat(1);
      if(th_flag_get(db, this, TH_Flag_Debug)) {
        shape->color = (V4){1, 0, 0, 1};
      }
      shape->sprite = Clamp(0, th_ivar_get(db, this, TH_IVar_Sprite), GM_Sprite_COUNT - 1);
    }
  }

  //- fp: the mark of the selection
  if(game->selection.kind != GM_SelectionKind_Nil &&
     rng2i32_contains(window, game->selection.tile)) {
    out.has_mark = true;
    out.mark = game->selection.tile;
  }

  arena_release_scratch(scratch);
  return out;
}


#include "game/board.c"
#include "game/worldgen.c"
#include "game/thing_db.c"
