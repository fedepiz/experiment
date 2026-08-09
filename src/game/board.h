#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "game/defs.h"

////////////////////////////////
//~ fp: Board
//
// The game board is a derived spatial index. It is never the authority. The
// truth is in the thing database, in its fields and its things. The board
// holds a copy of that truth: one terrain for each tile, features as
// connections between tiles, and pawns that stand on tiles. The shape of the
// copy suits spatial questions and pathfinding. You can compute the copy
// again from the truth at any time.
//
// The caller that writes the copy uses bd_tile_at, then clears the path
// cache.
//
// The board knows nothing more of the game. A terrain kind, a pawn kind and a
// pawn identity are numbers with no meaning here. The game gives them a
// meaning elsewhere.
//
// A coordinate is an integer tile position. Tile (0,0) is the top left corner,
// x increases to the right and y increases downward.
//
// All reads are total. A position out of bounds and an unknown handle both
// resolve to a shared nil object, which is read-only by convention. A chain of
// queries therefore cannot crash. Writes to pawns keep bookkeeping, so they go
// through functions. Those functions do nothing for a nil object or a position
// out of bounds.

////////////////////////////////
//~ fp: Features
//
// A feature is a line that the map draws over the terrain. A feature at a
// tile is a connection mask. Bit d is set when the feature continues toward
// neighbour d. A feature is present at a tile when its mask is not 0.
//
// A feature always connects. An object that stands at one tile and points in
// no direction is a pawn, and not a feature.

typedef U32 BD_Feature;
enum {
#define X(name, key) BD_Feature_##name,
  DF_FEATURE_LIST
#undef X
  BD_Feature_COUNT,
};

////////////////////////////////
//~ fp: Tiles
//
// A tile is one cell of the grid. Its terrain is a number with no meaning to
// the board. The board uses the number only as an index into the travel
// costs. The pawn place and pawn remove functions write the pawn list. Any
// caller can read it.

typedef U16 BD_Terrain;

typedef struct BD_Pawn BD_Pawn;
typedef struct {
  BD_Terrain terrain;
  U8 features[BD_Feature_COUNT]; // connection masks, bit d = toward Dir4 d

  // the pawns that stand here. Pawn place and pawn remove keep this list.
  BD_Pawn* first_pawn;
  BD_Pawn* last_pawn;
} BD_Tile;

////////////////////////////////
//~ fp: Pawns
//
// A pawn is an object that stands on a tile: a settlement, an army, a marker.
// The board gives a pawn a position and nothing more.
//
// The board does not make pawn identities. The caller supplies a U64 key,
// which has no meaning here. The game supplies its thing ids. bd_pawn_place
// writes the position of a known key and makes a pawn for an unknown key, so
// one call does both creation and movement. A lookup of an unknown key gives
// the nil pawn.

struct BD_Pawn {
  BD_Pawn* next; // in the pawn list of its tile; in the free list when removed
  BD_Pawn* prev;
  BD_Pawn* hash_next; // in the chain of its key bucket

  U64 key; // the identity that the caller gave this pawn. The board indexes
           // the key and reads nothing from it. The caller answers every
           // other question about the pawn from the key.
  V2I pos;
};

////////////////////////////////
//~ fp: Travel Rules
//
// The travel rules say how a movement cost reads the map. The board owns
// them, at board->rules. Write them one time. Every path query then agrees on
// the meaning of movement.
//
// The zero rules are valid (ZII). Each terrain then costs 1, and roads and
// rivers change nothing.

#define BD_TERRAIN_CAP 32

typedef struct {
  // The cost to enter a tile of terrain t is terrain_cost[t]. A cost of 0 or
  // less makes the terrain impassable. A terrain id of terrain_cost_count or
  // more is impassable. A count of 0 makes each terrain cost 1.
  //
  // The costs are in the structure and not behind a pointer. They are a copy
  // of the terrain data, so the A* search stays inside the board.
  F32 terrain_cost[BD_TERRAIN_CAP];
  U64 terrain_cost_count;

  // A road cost of more than 0 replaces the terrain cost of a step that
  // follows a road connection. Such a step also pays no river crossing cost,
  // because a road across a river is a bridge or a ford.
  F32 road_cost;

  // A river crossing cost of more than 0 is added to each step that enters a
  // tile with a river.
  F32 river_cross_cost;
} BD_TravelRules;

////////////////////////////////
//~ fp: Path Cache
//
// The cache keeps each path that the board computes, in an array of a fixed
// size. A search of the array is linear. The cache stops a repeated query from
// a second A* search. bd_path_next_towards is such a query: a caller uses it
// on each turn while an object walks.
//
// The hops of every path are in one shared pool of points. The board drops the
// whole cache when the array fills or the pool fills. It then builds the cache
// again on demand.
//
// The cache does not see a change to the map. Call bd_path_cache_clear after
// you write a tile or a rule. If you do not, the cache gives you an old path.
// The mirror of the game makes this call.

typedef struct {
  V2I from;
  V2I to;
  U32 first; // the first hop, in the point pool of the board
  U32 count; // hops, with both ends; a count of 0 means "no path"
  F32 cost;
} BD_PathEntry;

////////////////////////////////
//~ fp: Board State

typedef struct {
  Arena* arena; // the memory of the board: tiles, pawns and the path cache
  I32 width;
  I32 height;
  BD_Tile* tiles; // width * height tiles, row by row

  BD_TravelRules rules;

  BD_Pawn* first_free_pawn; // removed pawns, which place uses again
  U64 pawn_count;           // the pawns that stand on the board
  BD_Pawn** pawn_buckets;   // [pawn_bucket_count] key to pawn, chained by hash_next
  U64 pawn_bucket_count;    // a power of two, set at allocation

  //- fp: the path cache. Its capacity is set at allocation.
  BD_PathEntry* entries; // [entry_cap]
  U32 entry_cap;
  U32 entry_count;
  V2I* points; // the shared pool of hops. Each entry points into it.
  U64 point_count;
  U64 point_cap; // width * height, so one path always fits
} BD_Board;

////////////////////////////////
//~ fp: Nil
//
// A lookup out of bounds and a lookup of an unknown key resolve to one of
// these shared objects. A zeroed object is a nil object (ZII), so they need no
// initialization. They are read-only by convention. Never write through a nil
// object.

global BD_Tile BD_NIL_TILE;
global BD_Pawn BD_NIL_PAWN;

////////////////////////////////
//~ fp: Grid

internal BD_Board* bd_board_alloc(Arena* arena, I32 width, I32 height, U32 path_cache_entries);

internal B32 bd_in_bounds(BD_Board* board, V2I p);
internal BD_Tile* bd_tile_at(BD_Board* board, V2I p); // out of bounds: the nil tile

//- fp: distances in tile space. These need no board.
internal I32 bd_distance_steps(V2I a, V2I b); // moves with 4-way movement (manhattan)
internal F32 bd_distance(V2I a, V2I b);       // euclidean, in a straight line

////////////////////////////////
//~ fp: Disc Walks
//
// A disc walk gives you the tiles of the board within a euclidean radius of a
// center. Each question about a range needs this shape, so the board supplies
// it one time. The protocol is the protocol of the iterators of the thing
// database: construct, then loop on next.
//
//   for(BD_Disc it = bd_disc(board, center, range); bd_disc_next(&it);) {
//     ... it.pos, it.dist_sq ...
//   }
//
// The walk gives the tiles row by row, over the bounding box that it clamps to
// the board. Both radii are inclusive, as in the partition. Both must be 0 or
// more, which an assert checks. A center off the board and an empty band both
// give zero tiles, so no caller needs a guard for them.

typedef struct {
  V2I pos;     // the tile of the walk
  I32 dist_sq; // its squared distance from the center. Take the square root
               // only when you need the true distance. A walk that does not
               // need it pays nothing.

  //- fp: internals
  V2I center;
  I32 x0, y0, x1, y1; // the bounding box, clamped to the board
  F32 min_sq, max_sq; // the band, squared
} BD_Disc;

internal BD_Disc bd_disc(BD_Board* board, V2I center, F32 radius);
// a ring: the tiles at radius_min or more from the center
internal BD_Disc bd_disc_ring(BD_Board* board, V2I center, F32 radius_min, F32 radius_max);
internal B32 bd_disc_next(BD_Disc* it);

// The largest number of tiles that a walk can give, which is its bounding box.
// Use it when you must allocate before you walk. An empty walk gives 0.
internal U64 bd_disc_bound(BD_Disc it);

////////////////////////////////
//~ fp: Features
//
// The masks are a copy of data that the board does not own. The writer that
// makes the copy keeps this rule: a connection from p toward d is also a
// connection from the neighbour of p toward the opposite of d. See
// wg_field_connect.

internal U8 bd_feature_mask(BD_Board* board, V2I p, BD_Feature feature); // present when the mask is not 0

////////////////////////////////
//~ fp: Pawns
//
// To read the pawns of one tile, start at bd_tile_at(...)->first_pawn, then
// follow ->next.

// place writes the position of a known key, and makes a pawn for an unknown
// key. It does nothing when the position is out of bounds.
internal void bd_pawn_place(BD_Board* board, U64 key, V2I pos);
internal void bd_pawn_remove(BD_Board* board, U64 key);
internal BD_Pawn* bd_pawn_lookup(BD_Board* board, U64 key); // unknown key: the nil pawn

typedef struct {
  BD_Pawn** elems;
  U64 count;
} BD_PawnArray;

// each pawn that stands in [min, max], with both corners, pushed on `arena`
internal BD_PawnArray bd_pawns_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max);

// each pawn on the board, pushed on `arena`. The order is not defined.
internal BD_PawnArray bd_pawns_all(Arena* arena, BD_Board* board);

////////////////////////////////
//~ fp: Terrain Queries
//
// A patch is a rectangle of terrain, copied out row by row. The board clamps
// the rectangle that you ask for. `min`, `width` and `height` then report the
// part that the patch covers. A rectangle that goes past an edge therefore
// comes back smaller, and the board adds no padding. A width or a height of 0
// means that the rectangle missed the board.

typedef struct {
  V2I min;   // the top left tile of the patch
  I32 width; // the size of the patch. elems holds width * height ids.
  I32 height;
  BD_Terrain* elems; // row by row: elems[y * width + x] is the terrain at min + (x, y)
} BD_TerrainPatch;

// the terrain in [min, max], with both corners, pushed on `arena`
internal BD_TerrainPatch bd_terrain_in_rect(Arena* arena, BD_Board* board, V2I min, V2I max);

////////////////////////////////
//~ fp: Pathfinding
//
// The board searches for a path with A*, under board->rules. It answers from
// the path cache, and it computes a path and writes it into the cache when the
// cache does not hold it. bd_path_find gives you the whole path.
// bd_path_next_towards gives you one step, which is the form that the game
// logic uses on each tick.

// the waypoints from `from` to `to`, with both ends, copied onto `arena`. A
// count of 0 means that there is no path.
typedef struct {
  V2I* points;
  U64 count;
  F32 cost; // the total cost, under board->rules
} BD_Path;

internal BD_Path bd_path_find(Arena* arena, BD_Board* board, V2I from, V2I to);

// The cost of one step from `from` to the adjacent tile `to`, under
// board->rules. The search pays this same cost for that hop, so an object that
// spends a budget of movement agrees with the route that A* chooses. A cost of
// 0 or less means that the step is not possible: the tile is out of bounds, or
// it is not adjacent, or it is impassable.
internal F32 bd_step_cost(BD_Board* board, V2I from, V2I to);

// Can an object enter the tile at `p`? This is the terrain part of
// bd_step_cost, as a yes or a no. A position out of bounds gives false.
internal B32 bd_tile_passable(BD_Board* board, V2I p);

// The passable tile that is nearest to `want`. The board searches outward,
// one ring at a time. It gives `want` itself when the whole board is
// impassable.
internal V2I bd_snap_passable(BD_Board* board, V2I want);

// The tile after `from` on the path to `to`. The board gives `from` itself
// when the object is at `to` already, and when there is no path. Compare the
// result with `from` to find out that the object does not move.
internal V2I bd_path_next_towards(BD_Board* board, V2I from, V2I to);

internal void bd_path_cache_clear(BD_Board* board);

////////////////////////////////
//~ fp: Partition
//
// A partition divides the board between weighted sources. Each source sends
// out a strength that decreases with distance. Each tile goes to the source
// that reaches it with the largest strength.
//
// A source is a pawn key. It sends its strength from the tile where that pawn
// stands. A key that the board does not know sends nothing.
//
// The distance is euclidean, and it ignores the terrain. A strength pays no
// travel cost, and impassable ground does not stop it.

typedef U8 BD_Falloff;

enum {
  BD_Falloff_None,      // the full strength to the range, and 0 past it
  BD_Falloff_Linear,    // strength * (1 - d/range)
  BD_Falloff_Quadratic, // strength * (1 - d/range)^2
};

typedef struct {
  U64 key;      // the pawn that sends this strength. An unknown key sends nothing.
  F32 range;    // the tiles that it reaches, inclusive. A range of 0 reaches
                // its own tile only. A range of less than 0 reaches nothing.
  F32 strength; // the strength at the tile of the source. A strength of 0 or
                // less reaches nothing.
  BD_Falloff falloff;
} BD_Source;

typedef struct {
  U64 count;
  BD_Source* elems;
} BD_SourceArray;

// The source that holds a tile. A tile reads back unassigned only when no
// source reaches it. When two sources reach a tile with an equal strength, a
// hash of the key against the tile chooses between them. A border therefore
// alternates between its two sources, tile by tile, and no tile falls to
// nobody. The hash reads the key and the tile and nothing more, so the same
// sources always give the same partition, in any order.
typedef struct {
  U64 key;      // key_unassigned only when no source reached this tile
  F32 strength; // the strength that won. It is 0 when the tile is unassigned.
} BD_PartitionCell;

typedef struct {
  BD_PartitionCell* cells_per_tile;
} BD_Partition;

// Divide every tile between `sources`. The result is width * height cells,
// pushed on `arena`, row by row: result[y * board->width + x].
internal BD_PartitionCell* bd_partition(Arena* arena, BD_Board* board, BD_SourceArray sources, U64 key_unassigned);
