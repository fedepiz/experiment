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
  Arena* arena; // the memory of the game. gm_init makes it at the first call,
                // and clears it for each new world.
  B32 initialised;
  B32 paused;
  U64 tick_num;
  TH_Db* db;
  BD_Board* board;
  F32 move_timer;
  GM_Selection selection;
} GM_Game;

// A caller must call wg_terrain_table_load first: the generator and the
// feeding rules read the terrain table.
internal void gm_init(GM_Game* game, U64 seed);
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

// The map items are what the display layer draws. They carry no meaning of the
// game: a shape does not say that it is a territory, a person or a boundary,
// because the display layer has no use for that and cannot keep it correct.
//
// There are two lists, and they part where a real seam is. A shape is a
// finished order to draw. The ground is not: it is what the art system of the
// display layer reads to choose the pieces of a tile, and only that layer
// knows those pieces.
//
// The display layer reads nothing but these lists.

// One tile of the window. A cell outside the board is a ring cell: it takes a
// boundary shape from its neighbours, and draws no ground of its own.
typedef struct {
  V2I pos;
  BD_Terrain neighbours[9];      // the 3x3 tiles around pos, row by row. A tile
                                 // off the board reads 0.
  U8 features[BD_Feature_COUNT]; // the connection masks at pos
} GM_MapGround;

// One shape across one tile, above the ground.
//
// The list is in the order of the draw, and a shape covers each shape before
// it. A position can appear more than one time: two things that reach one tile
// give that tile two shapes.
typedef struct {
  V2I pos;
  V4 color;         // the color of the shape, or the tint of its art. It is the
                    // color to draw, with its alpha, and not a color to adjust.
  GM_Sprite sprite; // the art of the shape. The nil sprite asks for no art, and
                    // draws the color across the whole tile.
  TH_Id id;         // chooses between the variants of the art. It is stable for
                    // the life of a thing, so the choice is stable too.
} GM_MapShape;

// The shapes are an unrolled list, because every reader walks them in order
// and no reader indexes them. A chunk holds a block of shapes, so a push never
// counts ahead, and a read still walks contiguous memory.
#define GM_MAP_SHAPE_CHUNK_CAP 256

typedef struct GM_MapShapeChunk GM_MapShapeChunk;
struct GM_MapShapeChunk {
  GM_MapShapeChunk* next;
  U64 count;
  GM_MapShape shapes[GM_MAP_SHAPE_CHUNK_CAP];
};

typedef struct {
  GM_MapShapeChunk* first;
  GM_MapShapeChunk* last;
  U64 total_count;
} GM_MapShapeList;

typedef struct {
  GM_MapGround* ground; // flat: the window gives its exact size in one multiply
  U64 ground_count;
  GM_MapShapeList shapes;
  // The tile of the selection, which the display layer marks above everything.
  // The mark takes a color of the theme, so the display layer owns how it
  // looks, and it is not a shape.
  B32 has_mark;
  V2I mark;
} GM_MapItems;

typedef U32 GM_MapModeFlags;
enum {
  GM_MapModeFlag_Pawns = (1 << 1),
  GM_MapModeFlag_Influence = (1 << 2),
};

// The mode asks for one ground item at each cell of the window, with both
// corners. The window can go one cell past the edge of the board, to give the
// boundary shapes at that edge an owner. The mode then asks for one surface
// item at each pawn that stands inside the window.
typedef struct {
  Rng2I32 window;
  GM_MapModeFlags flags;
} GM_MapMode;

internal GM_MapItems gm_map_items(Arena* arena, GM_Game* game, GM_MapMode mode);
