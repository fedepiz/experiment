#include "base/core.h"
#include "base/math.h"
#include "base/rng.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "game/board.h"

#include <math.h>

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
  // About 4 tiles for each bucket keeps the pawn chains short at any normal
  // density. A larger density makes the chains longer, and the table does not
  // fill up.
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
//~ fp: Disc Walks

internal BD_Disc bd_disc_ring(BD_Board* board, V2I center, F32 radius_min, F32 radius_max) {
  BD_Disc it = {0};
  it.center = center;
  // The square of a radius has no sign, so this function cannot read a
  // negative radius. A negative radius is a mistake of the caller.
  Assert(radius_min >= 0 && radius_max >= 0);
  it.min_sq = radius_min * radius_min;
  it.max_sq = radius_max * radius_max;

  // The walk reaches the bounding box of the outer radius only, so no walk
  // reads the whole board. The cast to an integer is exact here: with an
  // integer center, the largest |dx| in the band is floor(radius_max).
  I32 reach = (I32)radius_max;
  it.x0 = ClampBot(center.x - reach, 0);
  it.y0 = ClampBot(center.y - reach, 0);
  it.x1 = ClampTop(center.x + reach, board->width - 1);
  it.y1 = ClampTop(center.y + reach, board->height - 1);

  // one position before the first tile, so the first call to next reaches it
  it.pos = (V2I){it.x0 - 1, it.y0};
  return it;
}

internal BD_Disc bd_disc(BD_Board* board, V2I center, F32 radius) {
  return bd_disc_ring(board, center, 0, radius);
}

internal B32 bd_disc_next(BD_Disc* it) {
  for(;;) {
    it->pos.x += 1;
    if(it->pos.x > it->x1) {
      it->pos.x = it->x0;
      it->pos.y += 1;
    }
    // After the walk moves to the next row, an x that is past the end shows
    // that the box is empty on that axis. This one test therefore stops a
    // walk that is complete and a walk that never had a tile.
    if(it->pos.x > it->x1 || it->pos.y > it->y1) { return 0; }

    I32 dx = it->pos.x - it->center.x;
    I32 dy = it->pos.y - it->center.y;
    it->dist_sq = dx * dx + dy * dy;
    F32 dist_sq = (F32)it->dist_sq;
    if(dist_sq <= it->max_sq && dist_sq >= it->min_sq) { return 1; }
  }
}

internal U64 bd_disc_bound(BD_Disc it) {
  U64 result = 0;
  if(it.x0 <= it.x1 && it.y0 <= it.y1) {
    result = (U64)(it.x1 - it.x0 + 1) * (U64)(it.y1 - it.y0 + 1);
  }
  return result;
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

////////////////////////////////
//~ fp: Pawns

internal BD_Pawn** bd__pawn_bucket(BD_Board* board, U64 key) {
  // Mix the key first. Keys that follow each other would fill one bucket.
  return &board->pawn_buckets[rng_mix_u64(key) & (board->pawn_bucket_count - 1)];
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
  // Count, then fill. Two small passes are less work than storage that grows.
  U64 count = 0;
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      BD_Tile* tile = &board->tiles[(U64)y * board->width + x];
      for(BD_Pawn* pawn = tile->first_pawn; pawn != 0; pawn = pawn->next) {
        count += 1;
      }
    }
  }
  result.elems = push_array_no_zero(arena, BD_Pawn*, count);
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      BD_Tile* tile = &board->tiles[(U64)y * board->width + x];
      for(BD_Pawn* pawn = tile->first_pawn; pawn != 0; pawn = pawn->next) {
        result.elems[result.count] = pawn;
        result.count += 1;
      }
    }
  }
  return result;
}

internal BD_PawnArray bd_pawns_all(Arena* arena, BD_Board* board) {
  BD_PawnArray result = {0};
  result.elems = push_array_no_zero(arena, BD_Pawn*, board->pawn_count);
  for(U64 bucket = 0; bucket < board->pawn_bucket_count; bucket += 1) {
    for(BD_Pawn* pawn = board->pawn_buckets[bucket]; pawn != 0; pawn = pawn->hash_next) {
      result.elems[result.count] = pawn;
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
  result.elems = push_array_no_zero(arena, BD_Terrain, (U64)result.width * result.height);
  for(I32 y = y0; y <= y1; y += 1) {
    for(I32 x = x0; x <= x1; x += 1) {
      result.elems[(U64)(y - y0) * result.width + (x - x0)] =
          board->tiles[(U64)y * board->width + x].terrain;
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Pathfinding -- A* core
//
// The state of the search is on the scratch arena. The path is the only
// result that leaves the function.
//
// The open set is a binary heap that deletes late. When the search finds a
// better cost for a node, it pushes that node again and does not change the
// entry that is in the heap. It then ignores each entry that it pops with a
// cost that is no longer the best. The in-degree of the grid limits the number
// of pushes, so the heap array holds 4 * tiles + 1 entries from the start and
// never grows.

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

// The cost of a step from `from_tile` toward `dir` into `to_tile`, under the
// rules of the board. A cost of 0 or less means that the step is not possible.
internal F32 bd__step_cost(BD_Board* board, BD_Tile* from_tile, Dir4 dir, BD_Tile* to_tile) {
  BD_TravelRules* rules = &board->rules;
  F32 cost = 1.0f;
  if(rules->terrain_cost_count != 0) {
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

// The smallest cost that one step can have. The A* estimate uses it, and the
// estimate must never be too large. The result is 0 when the rules make every
// terrain impassable.
internal F32 bd__min_step_cost(BD_TravelRules* rules) {
  F32 result = 1.0f;
  if(rules->terrain_cost_count != 0) {
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

// Search with A*, under board->rules. The path goes on `arena`. A count of 0
// means that there is no path.
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
    for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
      V2I np = v2i_add(p, dir4_delta(dir));
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
    Dir4 dir = dir4_from_delta(v2i_sub(to, from));
    if(dir < Dir4_COUNT) {
      result = bd__step_cost(board, bd_tile_at(board, from), dir, bd_tile_at(board, to));
    }
  }
  return result;
}

internal B32 bd_tile_passable(BD_Board* board, V2I p) {
  B32 result = bd_in_bounds(board, p);
  if(result && board->rules.terrain_cost_count != 0) {
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

// Find the entry for (from, to), or compute it and put it in the cache. The
// result is 0 when either end is out of bounds. The pointer is valid until the
// next path query only, because that query can drop the whole cache to make
// space.
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
    // Copy the points. Do not point at the cache: a later query can drop it.
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

////////////////////////////////
//~ fp: Partition

// The strength of `source` at `dist` tiles away. The result is 0 past its
// range. The falloff runs to range + 1, so the last tile in the range still
// reads more than 0.
internal F32 bd__source_at(BD_Source* source, F32 dist) {
  if(dist > source->range) { return 0; }
  F32 t = dist / (source->range + 1.0f);
  F32 result = source->strength;
  switch(source->falloff) {
    case BD_Falloff_Linear: {
      result *= 1.0f - t;
    } break;
    case BD_Falloff_Quadratic: {
      F32 falloff = 1.0f - t;
      result *= falloff * falloff;
    } break;
    default: break; // BD_Falloff_None: flat out to the range
  }
  return result;
}

// The rank of a key at one tile, for the case where the strength cannot
// separate two sources. The lowest rank wins. The rank comes from the key and
// the tile and nothing more. The winner is therefore the same in any order of
// the sources, and the same at each new computation.
internal U64 bd__partition_tiebreak(U64 key, U64 idx) {
  return rng_hash_u64(idx, key);
}

internal BD_PartitionCell* bd_partition(Arena* arena, BD_Board* board, BD_SourceArray sources, U64 key_unassigned) {
  U64 tile_count = (U64)board->width * board->height;
  BD_PartitionCell* result = push_array_no_zero(arena, BD_PartitionCell, tile_count);
  for(U64 idx = 0; idx < tile_count; idx += 1) {
    result[idx] = (BD_PartitionCell){.key = key_unassigned};
  }

  for(U64 i = 0; i < sources.count; i += 1) {
    BD_Source* source = &sources.elems[i];
    if(source->range < 0 || source->strength <= 0) { continue; } // range 0 still claims its own tile
    BD_Pawn* pawn = bd_pawn_lookup(board, source->key);
    if(pawn == &BD_NIL_PAWN) { continue; }

    // The walk gives the tiles in the range and no others, so the falloff
    // below reads only the tiles that can give more than 0.
    for(BD_Disc it = bd_disc(board, pawn->pos, source->range); bd_disc_next(&it);) {
      F32 strength = bd__source_at(source, sqrtf((F32)it.dist_sq));
      if(strength <= 0) { continue; }
      U64 idx = (U64)it.pos.y * board->width + it.pos.x;
      // The strength decides. Two equal strengths go to the rank of the tile,
      // so a border alternates between its two sources.
      if(strength > result[idx].strength ||
         (strength == result[idx].strength &&
          bd__partition_tiebreak(source->key, idx) <
              bd__partition_tiebreak(result[idx].key, idx))) {
        result[idx].key = source->key;
        result[idx].strength = strength;
      }
    }
  }
  return result;
}
