#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Board
//
// The game board: a 2d tile grid plus everything positioned on it. The board
// owns spatial state and answers spatial questions -- terrain per tile,
// features (rivers, roads) as inter-tile connections, pawns standing on
// tiles, and pathfinding over all of the above. It knows nothing else of the
// game: terrain kinds, pawn kinds, and pawn identities are opaque numbers the
// game gives meaning to elsewhere (what a terrain looks like or what a pawn
// *is* are not board questions).
//
// Coordinates are integer tile positions, (0,0) top-left, x right, y down.
// Reads are total: out-of-bounds and stale-handle lookups resolve to shared
// nil sentinels (read-only by convention), so query chains never crash.
// Mutations that carry bookkeeping (features, pawns) go through functions,
// which no-op on anything nil or out of bounds; plain per-tile data (terrain)
// is written directly through bd_tile_at.

////////////////////////////////
//~ fp: Directions
//
// The four tile neighborhoods, clockwise from north -- movement and features
// are 4-connected, DF-style. Feature connection masks below index bits by
// these, so the order is load-bearing: opposite direction == +2 mod 4.

typedef U32 BD_Dir;
enum {
  BD_Dir_N,
  BD_Dir_E,
  BD_Dir_S,
  BD_Dir_W,
  BD_Dir_COUNT,
};

internal V2I    bd_dir_delta(BD_Dir dir);
internal BD_Dir bd_dir_opposite(BD_Dir dir);
internal BD_Dir bd_dir_from_delta(V2I delta); // BD_Dir_COUNT when delta isn't a single step

////////////////////////////////
//~ fp: Features
//
// Linear things drawn over the terrain. A feature on a tile is a connection
// mask: bit d set means "this feature continues toward neighbor d". Features
// are inherently connective -- presence is mask != 0; a point-like thing with
// no direction is a pawn, not a feature.

typedef U32 BD_Feature;
enum {
  BD_Feature_River,
  BD_Feature_Road,
  BD_Feature_COUNT,
};

////////////////////////////////
//~ fp: Tiles
//
// One cell of the grid. Terrain is an opaque id -- the board never interprets
// it beyond indexing travel costs with it. The pawn list is bookkept: written
// by pawn place/remove, read freely.

typedef U16 BD_Terrain;

typedef struct BD_Pawn BD_Pawn;
typedef struct {
  BD_Terrain terrain;
  U8 features[BD_Feature_COUNT]; // connection masks, bit d = toward BD_Dir d

  // pawns standing here, maintained by pawn place/remove
  BD_Pawn* first_pawn;
  BD_Pawn* last_pawn;
} BD_Tile;

////////////////////////////////
//~ fp: Pawns
//
// Anything that stands on a tile: settlements, armies, markers -- the board
// neither knows nor cares, it just positions them. The board does not mint
// pawn identities: pawns are keyed by a caller-supplied U64 (opaque here; the
// game passes its thing ids), and bd_pawn_place is an upsert, so create and
// move are the same call. Unknown keys look up to the nil pawn.

struct BD_Pawn {
  BD_Pawn* next; // in its tile's pawn list; freelist link while removed
  BD_Pawn* prev;
  BD_Pawn* hash_next; // in its key bucket's chain

  U64 key; // the caller's identity for this pawn; the board only indexes it.
           // Anything else about the pawn -- looks, kind, meaning -- is the
           // caller's to answer from this key.
  V2I pos;
};

////////////////////////////////
//~ fp: Travel Rules
//
// How movement costs read the map, owned by the board (board->rules): set
// them once and every path query agrees on what movement means. The zero
// rules are valid (ZII): every terrain costs 1, roads and rivers change
// nothing.

typedef struct {
  // cost to enter a tile of terrain t is terrain_cost[t]; <= 0 means
  // impassable, and ids >= terrain_cost_count are impassable too. A zero
  // pointer means every terrain costs 1.
  F32* terrain_cost;
  U64 terrain_cost_count;

  // when > 0 and the step follows a road connection, this replaces the
  // terrain cost -- and waives the river crossing below (a road over a river
  // is a bridge or ford)
  F32 road_cost;

  // when > 0, added to any step entering a tile a river runs through
  F32 river_cross_cost;
} BD_TravelRules;

////////////////////////////////
//~ fp: Path Cache
//
// Computed paths are remembered in a finite, linearly-searched array so that
// repeated queries -- above all bd_path_next_towards called every turn while
// something walks -- do not re-run A* each time. Hops live in one shared
// point pool; when either the entries or the pool fill up, the whole cache is
// dropped and rebuilds on demand.
//
// The cache never observes map mutations. Feature connect/disconnect clear it
// themselves; after writing terrain or rules directly, call
// bd_path_cache_clear or stale paths will be served.

typedef struct {
  V2I from;
  V2I to;
  U32 first; // into the board's point pool
  U32 count; // hops including both endpoints; 0 = remembered "no path"
  F32 cost;
} BD_PathEntry;

////////////////////////////////
//~ fp: Board State

typedef struct {
  Arena* arena; // all board memory lives here (tiles, pawns, path cache)
  I32 width;
  I32 height;
  BD_Tile* tiles; // width * height, row-major

  BD_TravelRules rules;

  BD_Pawn* first_free_pawn; // removed pawns, recycled by place
  U64 pawn_count;           // placed pawns
  BD_Pawn** pawn_buckets;   // [pawn_bucket_count] key -> pawn, chained by hash_next
  U64 pawn_bucket_count;    // power of two, sized at alloc

  //- fp: path cache, capacity chosen at alloc
  BD_PathEntry* entries; // [entry_cap]
  U32 entry_cap;
  U32 entry_count;
  V2I* points;      // shared hop pool; entries reference slices of it
  U64 point_count;
  U64 point_cap;    // width * height -- any single path fits
} BD_Board;

////////////////////////////////
//~ fp: Nil
//
// Shared sentinels that out-of-bounds / stale lookups resolve to. Zeroed ==
// nil (ZII), so these need no initialization. Read-only by convention --
// nothing may ever write through a nil.

global BD_Tile BD_NIL_TILE;
global BD_Pawn BD_NIL_PAWN;

////////////////////////////////
//~ fp: Grid

internal BD_Board* bd_board_alloc(Arena* arena, I32 width, I32 height, U32 path_cache_entries);

internal B32      bd_in_bounds(BD_Board* board, V2I p);
internal BD_Tile* bd_tile_at(BD_Board* board, V2I p); // out of bounds: the nil tile

//- fp: tile-space distances; no board needed
internal I32 bd_distance_steps(V2I a, V2I b); // moves under 4-way movement (manhattan)
internal F32 bd_distance(V2I a, V2I b);       // euclidean, "as the crow flies"

////////////////////////////////
//~ fp: Features
//
// Connections are kept mirrored: connecting p toward d also connects p's
// neighbor toward opposite(d). At the map edge the neighbor half is simply
// dropped -- a river may flow off the world.

internal U8   bd_feature_mask(BD_Board* board, V2I p, BD_Feature feature); // presence = mask != 0
internal void bd_feature_connect(BD_Board* board, V2I p, BD_Dir dir, BD_Feature feature);
internal void bd_feature_disconnect(BD_Board* board, V2I p, BD_Dir dir, BD_Feature feature);

////////////////////////////////
//~ fp: Pawns
//
// Pawns on one tile read directly: bd_tile_at(...)->first_pawn, then ->next.

internal void      bd_pawn_place(BD_Board* board, U64 key, V2I pos); // upsert; no-op when pos is out of bounds
internal void      bd_pawn_remove(BD_Board* board, U64 key);
internal BD_Pawn*  bd_pawn_lookup(BD_Board* board, U64 key); // unknown key: the nil pawn

typedef struct {
  BD_Pawn** v;
  U64 count;
} BD_PawnArray;

// every alive pawn standing in [min, max] (corners inclusive), pushed on `arena`
internal BD_PawnArray bd_pawns_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max);

// every placed pawn, pushed on `arena`; order is arbitrary
internal BD_PawnArray bd_pawns_all(Arena* arena, BD_Board* board);

////////////////////////////////
//~ fp: Terrain Queries
//
// A rectangular window of terrain, copied out row-major. The requested rect
// is clamped to the board first; `min` and width/height describe what was
// actually covered, so a request hanging off the edge comes back smaller
// rather than padded. Zero width/height means the rect missed the board.

typedef struct {
  V2I min;    // top-left tile the window actually starts at
  I32 width;  // window dimensions; v holds width * height ids
  I32 height;
  BD_Terrain* v; // row-major: v[y * width + x] is terrain at min + (x, y)
} BD_TerrainPatch;

// terrain in [min, max] (corners inclusive), pushed on `arena`
internal BD_TerrainPatch bd_terrain_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max);

////////////////////////////////
//~ fp: Pathfinding
//
// A* under board->rules, answered from the path cache (computing and filling
// it on miss). bd_path_find hands back the whole path; bd_path_next_towards
// is the walk-one-step form game logic will call every tick.

// waypoints from `from` to `to`, both included, copied onto `arena`;
// count == 0 means no path
typedef struct {
  V2I* points;
  U64 count;
  F32 cost; // total, under board->rules
} BD_Path;

internal BD_Path bd_path_find(Arena* arena, BD_Board* board, V2I from, V2I to);

// cost of the single step from `from` onto the adjacent tile `to` under
// board->rules -- exactly what pathfinding pays for that hop, so movement
// that spends a budget agrees with the routes A* picks. <= 0 means the step
// cannot be taken (out of bounds, not adjacent, impassable).
internal F32 bd_step_cost(BD_Board* board, V2I from, V2I to);

// can the tile at `p` be entered at all -- the terrain half of bd_step_cost
// as a yes/no; false out of bounds
internal B32 bd_tile_passable(BD_Board* board, V2I p);

// nearest passable tile to `want`, searching outward ring by ring; `want`
// itself when the whole board is impassable
internal V2I bd_snap_passable(BD_Board* board, V2I want);

// the tile after `from` on the path to `to`; `from` itself when already
// there, or when no path exists (v2i_eq with `from` detects "not moving")
internal V2I bd_path_next_towards(BD_Board* board, V2I from, V2I to);

internal void bd_path_cache_clear(BD_Board* board);
