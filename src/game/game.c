////////////////////////////////
//~ fp: Game Layer -- implementations of the layers below, in dependency order

#include "game/game.h"
#include "base/core.h"
#include "base/math.h"
#include "base/tctx.h"
#include "game/board.h"
#include "game/thing_db.h"

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
#define GM_INFLUENCE_RANGE    8.0f
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

  //- fp: the world: generated into the fields, then mirrored into the board
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
  }

  // demo dressing: a band holding the land around it, so tiles have a Home
  {
    TH_Id id = th_spawn(db);
    gm__xy_set(db, id, (V2I){100, 100});
    th_ivar_set(db, id, TH_IVar_Sprite, GM_Sprite_Band);
    th_flag_set(db, id, TH_Flag_HasInfluence, true);
    th_flag_set(db, id, TH_Flag_Placed, true);
  }

  th_commit(db);
  gm__extract(game);
  gm__home_derive(game);
  game->initialised = true;
}

internal void gm_update(GM_Game* game, F32 dt) {
  TH_Db* db = game->db;
  game->move_timer = ClampTop(game->move_timer + dt, 0.5f);
  while(game->move_timer > 0.1f) {
    game->move_timer -= 0.1f;

    for(TH_Id this = th_first_flagged(db, TH_Flag_Placed);
        this != 0; this = th_next_flagged(db, TH_Flag_Placed, this)) {
      // 1 point per tick: a plains step; banking is capped so waiting at a
      // cheap stretch cannot buy a later teleport across an expensive one
      F32* pts = th_var(db, this, TH_Var_MovePts);
      *pts = ClampTop(*pts + 1.0f, 4.0f);

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

    th_commit(db);
    gm__extract(game);
    gm__home_derive(game);
  }
}

internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, V2I min, V2I max) {
  TH_Db* db = game->db;
  BD_Board* board = game->board;
  GM_MapItems out = {0};
  if(max.x < min.x || max.y < min.y) { return out; }

  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  BD_PawnArray pawns = bd_pawns_all(scratch.arena, board);
  U64 cell_count = (U64)(max.x - min.x + 1) * (U64)(max.y - min.y + 1);
  out.items = push_array(arena, GM_MapItem, cell_count + pawns.count);

  //- fp: ground segment: one item per window cell, off-board ring included
  for(I32 y = min.y; y <= max.y; y += 1) {
    for(I32 x = min.x; x <= max.x; x += 1) {
      GM_MapItem* item = &out.items[out.count];
      out.count += 1;
      item->pos = (V2I){x, y};
      item->has_terrain = bd_in_bounds(board, item->pos);

      TH_Id home = th_field_ref_get(db, TH_FieldRef_Home, item->pos);
      if(home) {
        item->color = (V4){1, 1, 0, 1};
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
  for(U64 idx = 0; idx < pawns.count; idx++) {
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

  arena_release_scratch(scratch);
  return out;
}


#include "game/board.c"
#include "game/worldgen.c"
#include "game/thing_db.c"
