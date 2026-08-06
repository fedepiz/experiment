#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "game/board.h"

#include <math.h>

////////////////////////////////
//~ fp: Directions

global V2I bd__dir_deltas[BD_Dir_COUNT] = {
    {0, -1},
    {1, 0},
    {0, 1},
    {-1, 0},
};

internal V2I bd_dir_delta(BD_Dir dir) {
  Assert(dir < BD_Dir_COUNT);
  return bd__dir_deltas[dir];
}

internal BD_Dir bd_dir_opposite(BD_Dir dir) {
  Assert(dir < BD_Dir_COUNT);
  return (dir + 2) % BD_Dir_COUNT;
}

internal BD_Dir bd_dir_from_delta(V2I delta) {
  BD_Dir result = BD_Dir_COUNT;
  for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
    if(v2i_eq(bd__dir_deltas[dir], delta)) {
      result = dir;
      break;
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Grid

internal BD_Board* bd_board_alloc(Arena* arena, I32 width, I32 height, U32 path_cache_entries) {
  Assert(width > 0 && height > 0 && path_cache_entries > 0);
  BD_Board* board = push_array(arena, BD_Board, 1);
  board->arena = arena;
  board->width = width;
  board->height = height;
  U64 tile_count = (U64)width * (U64)height;
  board->tiles = push_array(arena, BD_Tile, tile_count);
  board->entry_cap = path_cache_entries;
  board->entries = push_array_no_zero(arena, BD_PathEntry, board->entry_cap);
  board->points = push_array_no_zero(arena, V2I, tile_count);
  board->point_cap = tile_count;
  // ~4 tiles per bucket keeps pawn chains short at any sane density; chains
  // degrade gracefully rather than filling up beyond that
  board->pawn_bucket_count = 64;
  while(board->pawn_bucket_count * 4 < tile_count) { board->pawn_bucket_count *= 2; }
  board->pawn_buckets = push_array(arena, BD_Pawn*, board->pawn_bucket_count);
  return board;
}

internal B32 bd_in_bounds(BD_Board* board, V2I p) {
  return 0 <= p.x && p.x < board->width && 0 <= p.y && p.y < board->height;
}

internal BD_Tile* bd_tile_at(BD_Board* board, V2I p) {
  BD_Tile* result = &BD_NIL_TILE;
  if(bd_in_bounds(board, p)) {
    result = &board->tiles[(U64)p.y * board->width + p.x];
  }
  return result;
}

internal I32 bd_distance_steps(V2I a, V2I b) {
  I32 dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  I32 dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  return dx + dy;
}

internal F32 bd_distance(V2I a, V2I b) {
  F32 dx = (F32)(a.x - b.x);
  F32 dy = (F32)(a.y - b.y);
  return sqrtf(dx * dx + dy * dy);
}

////////////////////////////////
//~ fp: Features

internal U8 bd_feature_mask(BD_Board* board, V2I p, BD_Feature feature) {
  U8 result = 0;
  if(feature < BD_Feature_COUNT) {
    result = bd_tile_at(board, p)->features[feature];
  }
  return result;
}

internal void bd_feature_connect(BD_Board* board, V2I p, BD_Dir dir, BD_Feature feature) {
  if(feature >= BD_Feature_COUNT || dir >= BD_Dir_COUNT) { return; }
  BD_Tile* tile = bd_tile_at(board, p);
  if(tile == &BD_NIL_TILE) { return; }
  tile->features[feature] |= (U8)(1u << dir);
  BD_Tile* neighbor = bd_tile_at(board, v2i_add(p, bd__dir_deltas[dir]));
  if(neighbor != &BD_NIL_TILE) {
    neighbor->features[feature] |= (U8)(1u << bd_dir_opposite(dir));
  }
  bd_path_cache_clear(board);
}

internal void bd_feature_disconnect(BD_Board* board, V2I p, BD_Dir dir, BD_Feature feature) {
  if(feature >= BD_Feature_COUNT || dir >= BD_Dir_COUNT) { return; }
  BD_Tile* tile = bd_tile_at(board, p);
  if(tile == &BD_NIL_TILE) { return; }
  tile->features[feature] &= (U8) ~(1u << dir);
  BD_Tile* neighbor = bd_tile_at(board, v2i_add(p, bd__dir_deltas[dir]));
  if(neighbor != &BD_NIL_TILE) {
    neighbor->features[feature] &= (U8) ~(1u << bd_dir_opposite(dir));
  }
  bd_path_cache_clear(board);
}

////////////////////////////////
//~ fp: Pawns

// splitmix64 finalizer: spreads consecutive keys across the buckets
internal U64 bd__key_hash(U64 key) {
  key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ull;
  key ^= key >> 27; key *= 0x94d049bb133111ebull;
  key ^= key >> 31;
  return key;
}

internal BD_Pawn** bd__pawn_bucket(BD_Board* board, U64 key) {
  return &board->pawn_buckets[bd__key_hash(key) & (board->pawn_bucket_count - 1)];
}

internal BD_Pawn* bd_pawn_lookup(BD_Board* board, U64 key) {
  BD_Pawn* pawn = *bd__pawn_bucket(board, key);
  for(; pawn != 0 && pawn->key != key; pawn = pawn->hash_next) {}
  return pawn != 0 ? pawn : &BD_NIL_PAWN;
}

internal void bd_pawn_place(BD_Board* board, U64 key, V2I pos) {
  BD_Tile* dst = bd_tile_at(board, pos);
  if(dst == &BD_NIL_TILE) { return; }
  BD_Pawn* pawn = bd_pawn_lookup(board, key);
  if(pawn != &BD_NIL_PAWN) {
    BD_Tile* src = bd_tile_at(board, pawn->pos); // placed pawns are always in bounds
    DLLRemove(src->first_pawn, src->last_pawn, pawn);
  } else {
    pawn = board->first_free_pawn;
    if(pawn != 0) { SLLStackPop(board->first_free_pawn); MemoryZeroStruct(pawn); }
    else { pawn = push_array(board->arena, BD_Pawn, 1); }
    pawn->key = key;
    BD_Pawn** bucket = bd__pawn_bucket(board, key);
    pawn->hash_next = *bucket;
    *bucket = pawn;
    board->pawn_count += 1;
  }
  pawn->pos = pos;
  DLLPushBack(dst->first_pawn, dst->last_pawn, pawn);
}

internal void bd_pawn_remove(BD_Board* board, U64 key) {
  BD_Pawn** link = bd__pawn_bucket(board, key);
  for(; *link != 0 && (*link)->key != key; link = &(*link)->hash_next) {}
  BD_Pawn* pawn = *link;
  if(pawn == 0) { return; }
  *link = pawn->hash_next;
  BD_Tile* tile = bd_tile_at(board, pawn->pos); // placed pawns are always in bounds
  DLLRemove(tile->first_pawn, tile->last_pawn, pawn);
  SLLStackPush(board->first_free_pawn, pawn);
  board->pawn_count -= 1;
}

internal BD_PawnArray bd_pawns_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max) {
  BD_PawnArray result = {0};
  I32 x0 = ClampBot(min.x, 0);
  I32 y0 = ClampBot(min.y, 0);
  I32 x1 = ClampTop(max.x, board->width - 1);
  I32 y1 = ClampTop(max.y, board->height - 1);
  // count, then fill -- two cheap passes beat growable storage
  U64 count = 0;
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      BD_Tile* tile = &board->tiles[(U64)y * board->width + x];
      for(BD_Pawn* pawn = tile->first_pawn; pawn != 0; pawn = pawn->next) {
        count += 1;
      }
    }
  }
  result.v = push_array_no_zero(arena, BD_Pawn*, count);
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      BD_Tile* tile = &board->tiles[(U64)y * board->width + x];
      for(BD_Pawn* pawn = tile->first_pawn; pawn != 0; pawn = pawn->next) {
        result.v[result.count] = pawn;
        result.count += 1;
      }
    }
  }
  return result;
}

internal BD_PawnArray bd_pawns_all(Arena* arena, BD_Board* board) {
  BD_PawnArray result = {0};
  result.v = push_array_no_zero(arena, BD_Pawn*, board->pawn_count);
  for(U64 bucket = 0; bucket < board->pawn_bucket_count; bucket += 1) {
    for(BD_Pawn* pawn = board->pawn_buckets[bucket]; pawn != 0; pawn = pawn->hash_next) {
      result.v[result.count] = pawn;
      result.count += 1;
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Terrain Queries

internal BD_TerrainPatch bd_terrain_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max) {
  BD_TerrainPatch result = {0};
  I32 x0 = ClampBot(min.x, 0);
  I32 y0 = ClampBot(min.y, 0);
  I32 x1 = ClampTop(max.x, board->width - 1);
  I32 y1 = ClampTop(max.y, board->height - 1);
  if(x0 > x1 || y0 > y1) { return result; }
  result.min = (V2I){x0, y0};
  result.width = x1 - x0 + 1;
  result.height = y1 - y0 + 1;
  result.v = push_array_no_zero(arena, BD_Terrain, (U64)result.width * result.height);
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      result.v[(U64)(y - y0) * result.width + (x - x0)] =
          board->tiles[(U64)y * board->width + x].terrain;
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Pathfinding -- A* core
//
// Search state lives on scratch; only the reconstructed path escapes. The
// open set is a binary heap with lazy deletion: a relaxed node is re-pushed
// rather than re-keyed, and stale pops (g no longer the best known) are
// skipped. Pushes are bounded by the grid's in-degree, so the heap array is
// sized 4 * tiles + 1 up front and never grows.

typedef struct {
  F32 f;
  F32 g; // g at push time; the pop is stale when it no longer matches best_g
  U32 idx;
} BD__PathNode;

internal void bd__heap_push(BD__PathNode* heap, U64* count, BD__PathNode node) {
  U64 at = *count;
  *count += 1;
  heap[at] = node;
  while(at > 0) {
    U64 parent = (at - 1) / 2;
    if(heap[parent].f <= heap[at].f) { break; }
    Swap(BD__PathNode, heap[parent], heap[at]);
    at = parent;
  }
}

internal BD__PathNode bd__heap_pop(BD__PathNode* heap, U64* count) {
  BD__PathNode result = heap[0];
  *count -= 1;
  heap[0] = heap[*count];
  for(U64 at = 0;;) {
    U64 left = 2 * at + 1;
    U64 right = 2 * at + 2;
    U64 smallest = at;
    if(left < *count && heap[left].f < heap[smallest].f) { smallest = left; }
    if(right < *count && heap[right].f < heap[smallest].f) { smallest = right; }
    if(smallest == at) { break; }
    Swap(BD__PathNode, heap[at], heap[smallest]);
    at = smallest;
  }
  return result;
}

// cost of stepping from `from_tile` toward `dir` into `to_tile` under the
// board's rules; <= 0 means the step cannot be taken
internal F32 bd__step_cost(BD_Board* board, BD_Tile* from_tile, BD_Dir dir, BD_Tile* to_tile) {
  BD_TravelRules* rules = &board->rules;
  F32 cost = 1.0f;
  if(rules->terrain_cost != 0) {
    if(to_tile->terrain >= rules->terrain_cost_count) { return 0; }
    cost = rules->terrain_cost[to_tile->terrain];
    if(cost <= 0) { return 0; }
  }
  B32 on_road = (from_tile->features[BD_Feature_Road] >> dir) & 1;
  if(on_road && rules->road_cost > 0) {
    cost = rules->road_cost;
  } else if(to_tile->features[BD_Feature_River] != 0 && rules->river_cross_cost > 0) {
    cost += rules->river_cross_cost;
  }
  return cost;
}

// the cheapest any single step can be, for an admissible A* heuristic;
// 0 when the rules make everything impassable
internal F32 bd__min_step_cost(BD_TravelRules* rules) {
  F32 result = 1.0f;
  if(rules->terrain_cost != 0) {
    result = 0;
    for(U64 idx = 0; idx < rules->terrain_cost_count; idx += 1) {
      F32 cost = rules->terrain_cost[idx];
      if(cost > 0 && (result == 0 || cost < result)) { result = cost; }
    }
    if(result == 0) { return 0; }
  }
  if(rules->road_cost > 0 && rules->road_cost < result) { result = rules->road_cost; }
  return result;
}

internal F32 bd__heuristic(V2I a, V2I b, F32 min_cost) {
  return (F32)bd_distance_steps(a, b) * min_cost; // manhattan
}

// runs A* under board->rules; the path goes on `arena` (count 0 = no path)
internal BD_Path bd__path_compute(Arena* arena, BD_Board* board, V2I from, V2I to) {
  BD_Path result = {0};
  if(!bd_in_bounds(board, from) || !bd_in_bounds(board, to)) { return result; }
  if(v2i_eq(from, to)) {
    result.points = push_array_no_zero(arena, V2I, 1);
    result.points[0] = from;
    result.count = 1;
    return result;
  }
  BD_TravelRules* rules = &board->rules;
  F32 min_cost = bd__min_step_cost(rules);
  if(min_cost <= 0) { return result; }

  I32 w = board->width;
  U64 tile_count = (U64)w * board->height;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);

  F32* best_g = push_array_no_zero(scratch.arena, F32, tile_count);
  U32* parent = push_array_no_zero(scratch.arena, U32, tile_count);
  U8* state = push_array(scratch.arena, U8, tile_count); // 0 unseen, 1 open, 2 closed
  BD__PathNode* heap = push_array_no_zero(scratch.arena, BD__PathNode, tile_count * 4 + 1);
  U64 heap_count = 0;

  U32 start = (U32)((U64)from.y * w + from.x);
  U32 goal = (U32)((U64)to.y * w + to.x);
  best_g[start] = 0;
  parent[start] = start;
  state[start] = 1;
  bd__heap_push(heap, &heap_count,
                (BD__PathNode){bd__heuristic(from, to, min_cost), 0, start});

  B32 found = 0;
  while(heap_count > 0) {
    BD__PathNode node = bd__heap_pop(heap, &heap_count);
    if(state[node.idx] == 2 || node.g != best_g[node.idx]) { continue; } // stale duplicate
    state[node.idx] = 2;
    if(node.idx == goal) {
      found = 1;
      break;
    }

    V2I p = {(I32)(node.idx % w), (I32)(node.idx / w)};
    BD_Tile* tile = &board->tiles[node.idx];
    for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
      V2I np = v2i_add(p, bd__dir_deltas[dir]);
      if(!bd_in_bounds(board, np)) { continue; }
      U32 nidx = (U32)((U64)np.y * w + np.x);
      if(state[nidx] == 2) { continue; }
      F32 step = bd__step_cost(board, tile, dir, &board->tiles[nidx]);
      if(step <= 0) { continue; }
      F32 g = node.g + step;
      if(state[nidx] == 0 || g < best_g[nidx]) {
        best_g[nidx] = g;
        parent[nidx] = node.idx;
        state[nidx] = 1;
        bd__heap_push(heap, &heap_count,
                      (BD__PathNode){g + bd__heuristic(np, to, min_cost), g, nidx});
      }
    }
  }

  if(found) {
    U64 count = 1;
    for(U32 idx = goal; idx != start; idx = parent[idx]) { count += 1; }
    result.points = push_array_no_zero(arena, V2I, count);
    result.count = count;
    result.cost = best_g[goal];
    U64 at = count;
    for(U32 idx = goal;; idx = parent[idx]) {
      at -= 1;
      result.points[at] = (V2I){(I32)(idx % w), (I32)(idx / w)};
      if(idx == start) { break; }
    }
  }
  arena_release_scratch(scratch);
  return result;
}

internal F32 bd_step_cost(BD_Board* board, V2I from, V2I to) {
  F32 result = 0;
  if(bd_in_bounds(board, from) && bd_in_bounds(board, to)) {
    BD_Dir dir = bd_dir_from_delta(v2i_sub(to, from));
    if(dir < BD_Dir_COUNT) {
      result = bd__step_cost(board, bd_tile_at(board, from), dir, bd_tile_at(board, to));
    }
  }
  return result;
}

internal B32 bd_tile_passable(BD_Board* board, V2I p) {
  B32 result = bd_in_bounds(board, p);
  if(result && board->rules.terrain_cost != 0) {
    BD_Terrain terrain = bd_tile_at(board, p)->terrain;
    result = terrain < board->rules.terrain_cost_count &&
             board->rules.terrain_cost[terrain] > 0;
  }
  return result;
}

internal V2I bd_snap_passable(BD_Board* board, V2I want) {
  I32 max_radius = Max(board->width, board->height);
  for(I32 radius = 0; radius < max_radius; radius += 1) {
    for(I32 dy = -radius; dy <= radius; dy += 1) {
      for(I32 dx = -radius; dx <= radius; dx += 1) {
        if(Max(dx, -dx) != radius && Max(dy, -dy) != radius) { continue; } // ring, not disc
        V2I p = {want.x + dx, want.y + dy};
        if(bd_tile_passable(board, p)) { return p; }
      }
    }
  }
  return want;
}

////////////////////////////////
//~ fp: Pathfinding -- cache & public queries

internal void bd_path_cache_clear(BD_Board* board) {
  board->entry_count = 0;
  board->point_count = 0;
}

// find or compute-and-remember the (from, to) entry; 0 when either endpoint
// is out of bounds. The pointer is only good until the next path query -- it
// may drop the whole cache to make room.
internal BD_PathEntry* bd__path_entry_lookup(BD_Board* board, V2I from, V2I to) {
  if(!bd_in_bounds(board, from) || !bd_in_bounds(board, to)) { return 0; }
  for(U32 idx = 0; idx < board->entry_count; idx += 1) {
    BD_PathEntry* entry = &board->entries[idx];
    if(v2i_eq(entry->from, from) && v2i_eq(entry->to, to)) { return entry; }
  }

  ArenaTemp scratch = arena_get_scratch(0, 0);
  BD_Path path = bd__path_compute(scratch.arena, board, from, to);
  if(board->entry_count >= board->entry_cap ||
     board->point_count + path.count > board->point_cap) {
    bd_path_cache_clear(board); // full: drop everything, rebuild on demand
  }
  BD_PathEntry* entry = &board->entries[board->entry_count];
  board->entry_count += 1;
  entry->from = from;
  entry->to = to;
  entry->first = (U32)board->point_count;
  entry->count = (U32)path.count;
  entry->cost = path.cost;
  MemoryCopy(board->points + board->point_count, path.points, path.count * sizeof(V2I));
  board->point_count += path.count;
  arena_release_scratch(scratch);
  return entry;
}

internal BD_Path bd_path_find(Arena* arena, BD_Board* board, V2I from, V2I to) {
  BD_Path result = {0};
  BD_PathEntry* entry = bd__path_entry_lookup(board, from, to);
  if(entry != 0 && entry->count > 0) {
    // copied out, not aliased: the cache may drop everything on a later query
    result.points = push_array_no_zero(arena, V2I, entry->count);
    result.count = entry->count;
    result.cost = entry->cost;
    MemoryCopy(result.points, board->points + entry->first, entry->count * sizeof(V2I));
  }
  return result;
}

internal V2I bd_path_next_towards(BD_Board* board, V2I from, V2I to) {
  V2I result = from;
  BD_PathEntry* entry = bd__path_entry_lookup(board, from, to);
  if(entry != 0 && entry->count > 1) {
    result = board->points[entry->first + 1];
  }
  return result;
}
