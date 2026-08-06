#pragma once

////////////////////////////////
//~ fp: Game Layer -- headers of the layers below, in dependency order

#include "base/math.h"
#include "game/board.h"
#include "game/worldgen.h"
#include "game/thing_db.h"
#include "tabula.h"

////////////////////////////////
//~ fp: Game
//
// The pure simulated world: everything that would exist on a headless
// server. The thing database is the authoritative state -- things plus the
// world fields (terrain, feature masks); the board is derived from it, kept
// mirrored for spatial queries and pathfinding. A few entities wander a
// waypoint loop. Entities
// bank movement points each tick and pay the board's step cost to walk, so
// terrain speed is felt, not just routed around: forest crossings crawl,
// road hops fly. Time arrives as `dt` from the caller; nothing here touches
// the window, input, or drawing.

////////////////////////////////
//~ fp: Selection
//
// What the player currently has picked: nothing, a tile, or a thing. A thing
// selection stores the id; its tile re-derives every update, so the
// selection follows the thing and clears itself when the thing dies.

typedef U32 GM_SelectionKind;
enum {
  GM_SelectionKind_Nil, // nothing selected (ZII)
  GM_SelectionKind_Tile,
  GM_SelectionKind_Thing,
};

typedef struct {
  GM_SelectionKind kind;
  V2I tile; // Tile: the picked cell; Thing: the thing's tile as of the last update
  TH_Id id; // Thing only
} GM_Selection;

typedef struct {
  B32 initialised;
  TH_Db* db;
  BD_Board* board;
  F32 move_timer;
  GM_Selection selection;
} GM_Game;

internal void gm_init(Arena* arena, GM_Game* game, U64 seed);
internal void gm_update(GM_Game* game, F32 dt);

// a thing standing on the tile beats the tile itself; off-board clears
internal void gm_select(GM_Game* game, V2I tile);
internal void gm_deselect(GM_Game* game);

// facts about the current selection as a tabula tree on `arena`: kind, name,
// the tile's terrain and features, ... Consumers query by key (tb_get) and
// decide presentation themselves. Nil when nothing is selected.
internal TB_Value* gm_selection_info(Arena* arena, GM_Game* game);

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

// display identifier per sprite kind, indexed by the enum
global String8 GM_SPRITE_NAMES[GM_Sprite_COUNT] = {
    str8_lit_comp("nil"),     str8_lit_comp("band"),  str8_lit_comp("village"),
    str8_lit_comp("palace"),  str8_lit_comp("herders"), str8_lit_comp("wagon"),
    str8_lit_comp("tholos"),
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
  // selection marker on pos, drawn above everything; how it looks is the
  // renderer's call
  B8 has_highlight;
} GM_MapItem;

typedef struct {
  U64 count;
  GM_MapItem* items;
} GM_MapItems;

// ground items for every cell of [min, max] (corners inclusive; the window
// may extend one past the board edge for the far-edge boundary duals), then
// surface items for the pawns standing inside it
internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, V2I min, V2I max);
