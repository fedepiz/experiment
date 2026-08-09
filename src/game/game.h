#pragma once

////////////////////////////////
//~ fp: Game Layer -- the headers of the layers below, in the order of their use

#include "base/core.h"
#include "base/math.h"
#include "game/board.h"
#include "game/worldgen.h"
#include "game/thing_db.h"
#include "tabula.h"

////////////////////////////////
//~ fp: Game
//
// The game layer is the simulated world, and nothing more. It is each part
// that a server with no display would hold.
//
// The thing database is the authority. It holds the things and the fields of
// the world, which are the terrain and the feature masks. The board comes from
// the database. The game keeps the board equal to the database, for the
// spatial queries and for pathfinding.
//
// Some things walk a loop of waypoints. Each one collects movement points on
// each tick, and pays the step cost of the board to move. A group therefore
// feels the speed of a terrain and does not only go around it: a step into a
// forest is slow, and a step along a road is fast.
//
// The caller supplies the time as `dt`. This layer reads no window, no input
// and no drawing.

// The database column that holds the connection mask of each board feature.
// One list makes this table and both enums, so the mirror below names no
// feature.
global TH_IField GM_FEATURE_IFIELDS[BD_Feature_COUNT] = {
#define X(name, key) TH_IField_##name##Mask,
    DF_FEATURE_LIST
#undef X
};

// the name of each feature to show, and its art prefix, as with the sprites
global String8 GM_FEATURE_NAMES[BD_Feature_COUNT] = {
#define X(name, key) str8_lit_comp(key),
    DF_FEATURE_LIST
#undef X
};

////////////////////////////////
//~ fp: Selection
//
// The selection is what the player has chosen: nothing, a tile, or a thing. A
// selection of a thing holds the id of that thing. The game computes its tile
// again at each update, so the selection follows the thing, and the game
// clears the selection when the thing stops to exist.

typedef U32 GM_SelectionKind;
enum {
  GM_SelectionKind_Nil, // the player chose nothing (ZII)
  GM_SelectionKind_Tile,
  GM_SelectionKind_Thing,
};

typedef struct {
  GM_SelectionKind kind;
  V2I tile; // Tile: the cell that the player chose. Thing: the tile of the
            // thing, as of the last update.
  TH_Id id; // Thing only
} GM_Selection;

typedef struct {
  B32 initialised;
  B32 paused;
  U64 tick_num;
  TH_Db* db;
  BD_Board* board;
  F32 move_timer;
  GM_Selection selection;
} GM_Game;

internal void gm_init(Arena* arena, GM_Game* game, U64 seed);
internal void gm_update(GM_Game* game, F32 dt);

// A thing that stands on the tile wins against the tile. A tile off the board
// clears the selection.
internal void gm_select(GM_Game* game, V2I tile);
internal void gm_deselect(GM_Game* game);

// the facts of the game, as a tabula
internal TB_Value* gm_info(Arena* arena, GM_Game* game);

typedef U8 GM_Sprite;
enum {
  GM_Sprite_Nil,
#define X(name, key) GM_Sprite_##name,
  DF_SPRITE_LIST
#undef X
  GM_Sprite_COUNT,
};

// The name of each sprite to show, at the index of the enum. It is also the
// art prefix that the client reads, which is the same text.
global String8 GM_SPRITE_NAMES[GM_Sprite_COUNT] = {
    str8_lit_comp("nil"),
#define X(name, key) str8_lit_comp(key),
    DF_SPRITE_LIST
#undef X
};

// A map item is one entry of the stream that the display layer draws. That
// layer draws from these items alone, in the order of the list: first the
// ground cells, then the objects that stand on them. Each item is complete: a
// cell carries the terrain of its neighbours. The display layer therefore
// reads nothing but the list.
typedef struct {
  V2I pos;
  V4 color;
  // The ground cell. has_terrain is false at a ring cell outside the board.
  // Such a cell takes a boundary shape from its neighbours, and draws no
  // ground of its own.
  B8 has_terrain;
  BD_Terrain neighbours[9];      // the 3x3 tiles around pos, row by row. A tile
                                 // off the board reads 0.
  U8 features[BD_Feature_COUNT]; // the connection masks at pos
  // the pawn that stands at pos, which the display layer draws above the ground
  B8 has_pawn;
  GM_Sprite sprite;
  TH_Id id; // the thing. Its identity is stable, which suits a choice of art.
  // The mark of the selection at pos, which the display layer draws above
  // everything. The display layer chooses how the mark looks.
  B8 has_highlight;
} GM_MapItem;

typedef struct {
  U64 count;
  GM_MapItem* items;
} GM_MapItems;

typedef U32 GM_MapModeFlags;
enum {
  GM_MapModeFlag_Pawns = (1 << 1),
  GM_MapModeFlag_Influence = (1 << 2),
};

// The mode asks for one ground item at each cell of [min, max], with both
// corners. The window can go one cell past the edge of the board, to give the
// boundary shapes at that edge an owner. The mode then asks for one surface
// item at each pawn that stands inside the window.
typedef struct {
  V2 min;
  V2 max;
  GM_MapModeFlags flags;
} GM_MapMode;

internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, GM_MapMode mode);
