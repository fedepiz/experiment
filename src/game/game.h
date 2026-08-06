#pragma once

////////////////////////////////
//~ fp: Game Layer -- headers of the layers below, in dependency order

#include "base/math.h"
#include "game/board.h"
#include "game/worldgen.h"
#include "game/thing_db.h"

////////////////////////////////
//~ fp: Game
//
// The pure simulated world: everything that would exist on a headless
// server. The board plus a few entities wandering a waypoint loop. Entities
// bank movement points each tick and pay the board's step cost to walk, so
// terrain speed is felt, not just routed around: forest crossings crawl,
// road hops fly. Time arrives as `dt` from the caller; nothing here touches
// the window, input, or drawing.

typedef struct {
  B32 initialised;
  TH_Db* db;
  BD_Board* board;
  F32 move_timer;
} GM_Game;

internal void gm_init(Arena* arena, GM_Game* game, U64 seed);
internal void gm_update(GM_Game* game, F32 dt);

typedef U8 GM_Sprite;
enum {
  GM_Sprite_Nil,
  GM_Sprite_Band,
  GM_Sprite_Village,
  GM_Sprite_Palace,
  GM_Sprite_Herders,
  GM_Sprite_Wagon,
  GM_Sprite_Tholos,
  GM_Sprite_COUNT,
};

// Fat draw-stream entries for the presentation layer: it renders from these
// alone, in list order -- ground cells first, then what stands on them. Each
// item is self-contained (a cell brings its own neighbourhood), so drawing
// reads nothing beyond the list.
typedef struct {
  V2I pos;
  V4 color;
  // ground cell. has_terrain is false on the off-board ring cells, which
  // receive boundary spill but paint no ground of their own.
  B8 has_terrain;
  BD_Terrain neighbours[9]; // 3x3 around pos, row-major; off-board reads 0
  U8 features[BD_Feature_COUNT]; // connection masks at pos
  // pawn standing on pos, drawn on top
  B8 has_pawn;
  GM_Sprite sprite;
  TH_Id id; // the thing itself: stable identity for art variation and the like
} GM_MapItem;

typedef struct {
  U64 count;
  GM_MapItem* items;
} GM_MapItems;

// ground items for every cell of [min, max] (corners inclusive; the window
// may extend one past the board edge for the far-edge boundary duals), then
// surface items for the pawns standing inside it
internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, V2I min, V2I max);
