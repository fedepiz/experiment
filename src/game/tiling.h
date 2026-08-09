#pragma once

#include "base/core.h"
#include "base/math.h"

////////////////////////////////
//~ fp: Tiling -- the appearance of a map, over classes of cells
//
// The caller declares its classes, which are terrains in this game. It then
// registers its art by id. An id is a U32 that the caller makes, and it has no
// meaning here. In this game an id is a slot of the sprite table.
//
// A registration is one fact, and the calls can come in any order. "The ground
// window (2,3) of this class is id 17" is one such fact, and "case 6 has this
// mask" is another. Each call writes one entry of a table. A second call with
// the same fact writes over the first. No set of facts must be complete, and
// no order is necessary.
//
// For each cell, the caller then gives the 3x3 window of classes around that
// cell. It receives the pieces to draw: an art id, a mask id that can be
// absent, a layer, and an offset.
//
// This module holds each rule of appearance: the index into the ground of a
// torus, the choice between variants, the density of the overlays, the shape
// at a border, and the boundaries of the dual grid. The module sees no pixel,
// and the caller sees no rule.
//
// tl_cell is pure. The same config, the same window and the same position give
// the same answer. It holds no state, and it reads no asset and no board.
//
//- fp: The map draws the ground on the dual grid. That grid is half a cell
//  away from the map grid, so the center of each dual cell is a point where
//  four map cells meet.
//
//  A dual cell paints its own area in full. Nothing paints outside its cell,
//  so no piece covers another piece. Each map cell owns the dual cell at its
//  north west corner, at the offset {-0.5,-0.5}. The draw loop of the caller
//  must go one row and one column past the right edge and the bottom edge of
//  the view, so that each dual cell at an edge has an owner.
//
//  Inside a dual cell, the classes stack in the order that they cover each
//  other. The class with the lowest rank draws its ground window in full, as
//  if there were no border. A cell with one class is that piece alone.
//
//  Each class with a higher rank then draws its own ground through an alpha
//  mask. The shape of that mask covers the corners whose class has a rank at
//  or above the rank of this class. Two classes with an equal rank compare
//  their class ids.
//
//  A contour therefore stays inside the contour below it, and two contours
//  never cross. Contours also meet across two dual cells, because of how this
//  module builds them.
//
//  A transition is never one piece of art. It is always a ground id with a
//  mask id, and the renderer multiplies the two. The art therefore stays in
//  parts. The case of a masked layer is always one of the 14 cases below. A
//  class with no ground has nothing to spill, so the module skips it.
//
//  The module cuts a ground window on the offset grid. Window (gx,gy) is the
//  crop of the torus at (gx+0.5, gy+0.5) cells. The window of a dual cell is
//  therefore the part of the world under that cell. Two adjacent cells, with
//  one class or with more, continue one texture for each class with no seam.
//
//- fp: The cases, which are the index of the mask table. A case is a code of
//  4 bits, which are the TL_Corner_* bits. The 14 cases become 4 shapes under
//  rotation, and those 4 shapes are the mask art that a generator makes.
//
//   corner  1,2,4,8    one corner is covered. The contour is a quarter circle.
//   half    3,5,10,12  two adjacent corners. The contour is near to straight.
//   saddle  6,9        two opposite corners, joined by a diagonal neck.
//                      Movement has 8 directions, and the art agrees.
//   inner   7,11,13,14 three corners. It is the opposite of a quarter circle.
//
//  The rule for the generator of the masks: a contour enters and leaves a dual
//  cell at the middle of an edge. Between those two points the contour can
//  take any path. This rule is what makes the contours of two adjacent dual
//  cells meet.
//
//- fp: A network is a line feature, such as a river or a road. The module
//  chooses its art from a connection mask of 4 bits, where bit d is the
//  direction Dir4 d. The board supplies that mask, and the module does not
//  change it.
//
//  The art of a network is one finished piece for each connection case. A
//  piece enters and leaves at the middle of an edge, so two pieces join across
//  two cells.
//
//  The module draws the networks in the order of their ids, so a road above a
//  river reads as a bridge. A cell with any network draws no overlay, because
//  nothing stands on a river or on a road.
//
//- fp: The rule for the caller that draws. A piece can cover a part of an
//  adjacent cell, because a boundary piece covers parts of four map cells. The
//  caller must therefore draw the whole view one time for each TL_Layer, in
//  the order of the layers. Inside one layer, the order of the cells along a
//  row is correct.

#define TL_TORUS_GRID  4
#define TL_CLASS_CAP   24
#define TL_NETWORK_CAP 4
#define TL_VARIANT_CAP 8

// The corner bits of a boundary case. The name of a bit says where its corner
// is in the dual cell, which covers parts of four map cells.
enum {
  TL_Corner_NW = (1 << 0),
  TL_Corner_NE = (1 << 1),
  TL_Corner_SW = (1 << 2),
  TL_Corner_SE = (1 << 3),
};

// tl_class_set and the tl_push_* calls fill this structure. Read the result of
// tl_cell, and do not read these members.
typedef struct {
  U8  rank;            // the order at a border. A higher rank covers a lower one.
  U32 overlay_density; // the percent of the inner cells that carry an overlay
  U32 ground_ids[TL_TORUS_GRID][TL_TORUS_GRID]; // [gy][gx], the windows of the offset grid
  U32 overlay_ids[TL_VARIANT_CAP];              // the art inside a region
  U32 overlay_count;
  U32 edge_overlay_ids[TL_VARIANT_CAP];         // the art at the border of a region
  U32 edge_overlay_count;
} TL_Class;

// A config of all zeros is valid and empty (ZII). Write the seed directly. The
// calls below supply each other value. class_count is the largest class id
// that a call named. The module ignores an id at TL_CLASS_CAP or above, so no
// caller can make a mistake here, and no caller must set a size first.
typedef struct {
  U64 seed; // The module adds this to the hash of a position, so one world can
            // take a different style.
  U32 class_count;
  TL_Class classes[TL_CLASS_CAP];
  U32 mask_ids[16][TL_VARIANT_CAP]; // by case code. One set serves each class.
  U32 mask_counts[16];
  U32 network_ids[TL_NETWORK_CAP][16][TL_VARIANT_CAP]; // by connection case
  U32 network_counts[TL_NETWORK_CAP][16];
} TL_Config;

internal void tl_class_set(TL_Config* config, U32 klass, U8 rank, U32 overlay_density);
internal void tl_push_ground(TL_Config* config, U32 klass, U32 gx, U32 gy, U32 id);
internal void tl_push_overlay(TL_Config* config, U32 klass, U32 id);
internal void tl_push_edge_overlay(TL_Config* config, U32 klass, U32 id);
internal void tl_push_mask(TL_Config* config, U32 code, U32 id);
internal void tl_push_network(TL_Config* config, U32 network, U32 code, U32 id);

typedef U32 TL_Layer;
enum {
  TL_Layer_Surface, // the ground and the boundary shapes. Draw this layer first.
  TL_Layer_Network, // the rivers and the roads: above the ground, below the objects
  TL_Layer_Object,  // the overlays, which the map draws above each surface
  TL_Layer_COUNT,
};

typedef struct {
  U32 id;         // an art id that a call registered. 0 asks the caller for its own art.
  U32 mask_id;    // the alpha mask to draw `id` through. 0 means no mask.
  U32 klass;      // the class of the piece, for a color and for a diagnostic
  TL_Layer layer;
  V2  offset;     // the offset from the origin of the cell, in cell units
} TL_Piece;

typedef struct {
  TL_Piece pieces[8]; // the order to draw inside one cell. Across the cells,
                      // draw one layer at a time.
  U32 count;
} TL_Cell;

// `neighborhood` is the 3x3 window of classes around the cell, row by row.
// Index 4 is the cell itself. A class id at class_count or above reads as
// class 0, which is the diagnostic for a wrong id.
//
// `networks` is the connection mask of the cell for each network id. A value
// of 0 means no network (ZII).
//
// `p` is the position of the cell in the world.
internal TL_Cell tl_cell(TL_Config* config, U32 neighborhood[9],
                         U8 networks[TL_NETWORK_CAP], V2I p);
