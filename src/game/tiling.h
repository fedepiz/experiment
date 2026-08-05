#pragma once

#include "base/core.h"
#include "base/math.h"

////////////////////////////////
//~ fp: Tiling -- pure map-appearance decisions over abstract cell classes
//
// The caller declares its classes (terrains, in practice) and registers its
// art by id: opaque U32s the caller mints (sprite-table slots, in practice).
// Registration is a stream of facts in any order -- "this class's ground
// window (2,3) is id 17", "case 6 has this mask" -- each an independent
// table write; re-stating a fact overwrites it, and nothing needs to be
// complete or ordered. Per cell, the caller then hands over the cell's 3x3
// class neighborhood and gets back the pieces to draw: art id, optional
// mask id, layer, offset. All appearance policy lives here: torus ground
// indexing, variant noise, overlay density, border tapering, and dual-grid
// marching-squares boundaries. The module never sees pixels, and the caller
// never sees policy.
//
// tl_cell is pure: same config, same neighborhood, same position -- same
// answer. No hidden state, no assets, no board.
//
//- Boundaries are drawn as MASKED GROUND, not baked transition art: a
// boundary piece pairs one window of the spilling class's own ground (id)
// with a case-shaped alpha mask (mask_id), and the renderer multiplies the
// two. Assets stay factored -- grounds and masks, never their product.
//
//- The dual grid: the drawn boundary grid is offset half a cell from the
// map grid; each DUAL CELL is centered on a point where four map cells meet
// and shows the piece of boundary passing through it. Each map cell owns
// the dual cell at its NW corner (offset {-0.5,-0.5}); the caller's draw
// loop must extend one row/column past the viewport's right/bottom so edge
// dual cells find an owner.
//
// Boundary layers are cumulative fields: the layer for class c covers the
// corners whose class ranks at-or-above c (rank ties break by class id), so
// contours nest, never cross, and connect across neighboring dual cells by
// construction. The lowest-ranked class present is the background and emits
// no layer; a class spills only if it outranks the background and has
// ground to spill. A non-background layer is always one of the 14
// non-trivial cases below.
//
//- Case taxonomy (the mask registry's index): a 4-bit corner code,
// TL_Corner_* bits. The 14 cases collapse under rotation into 4 canonical
// shapes, which is all the mask art a generator authors:
//
//   corner  1,2,4,8    one corner covered; quarter arc
//   half    3,5,10,12  two adjacent corners; straight-ish contour
//   saddle  6,9        two opposite corners; CONNECTED by a diagonal neck
//                      (movement is 8-directional, the art agrees)
//   inner   7,11,13,14 three corners; complement of a quarter arc
//
// Mask contract for the generator: contours enter and exit dual cells at
// EDGE MIDPOINTS (wander is free in between), which is what makes contours
// of neighboring dual cells meet.
//
//- Draw contract: pieces overlap across cells (boundary pieces straddle
// four map cells), so the caller draws the viewport once per TL_Layer, in
// layer order: every Surface piece of every cell, then every Object piece.
// Within a layer, cell scanline order is correct by construction.

#define TL_TORUS_GRID  4
#define TL_CLASS_CAP   16
#define TL_VARIANT_CAP 8

// corner bits of a boundary case, named for where the corner sits in the
// dual cell (which straddles four map cells)
enum {
  TL_Corner_NW = (1 << 0),
  TL_Corner_NE = (1 << 1),
  TL_Corner_SW = (1 << 2),
  TL_Corner_SE = (1 << 3),
};

// filled by tl_class_set / tl_push_*; read tl_cell's output, not these
typedef struct {
  U8  rank;            // boundary covering order; higher spills over lower
  U32 overlay_density; // percent of interior cells that carry an overlay
  U32 ground_ids[TL_TORUS_GRID][TL_TORUS_GRID]; // [gy][gx] torus windows
  U32 overlay_ids[TL_VARIANT_CAP];              // interior pictographic art
  U32 overlay_count;
  U32 edge_overlay_ids[TL_VARIANT_CAP];         // sparse region-border art
  U32 edge_overlay_count;
} TL_Class;

// ZII: zero is a valid empty config. Set seed directly; everything else
// arrives via the calls below. class_count tracks the highest class id
// mentioned; ids at-or-past TL_CLASS_CAP are ignored (nothing to get wrong,
// nothing to pre-size).
typedef struct {
  U64 seed; // folded into the position hash, so worlds can restyle
  U32 class_count;
  TL_Class classes[TL_CLASS_CAP];
  U32 mask_ids[16][TL_VARIANT_CAP]; // by case code; global, class-agnostic
  U32 mask_counts[16];
} TL_Config;

internal void tl_class_set(TL_Config* config, U32 klass, U8 rank, U32 overlay_density);
internal void tl_push_ground(TL_Config* config, U32 klass, U32 gx, U32 gy, U32 id);
internal void tl_push_overlay(TL_Config* config, U32 klass, U32 id);
internal void tl_push_edge_overlay(TL_Config* config, U32 klass, U32 id);
internal void tl_push_mask(TL_Config* config, U32 code, U32 id);

typedef U32 TL_Layer;
enum {
  TL_Layer_Surface, // ground and boundary spill; drawn first
  TL_Layer_Object,  // standing art (overlays); drawn over every surface
  TL_Layer_COUNT,
};

typedef struct {
  U32 id;         // registered art id; 0 = caller's fallback path
  U32 mask_id;    // alpha mask to draw `id` through; 0 = unmasked
  U32 klass;      // the class the piece belongs to (fallback color, debug)
  TL_Layer layer;
  V2  offset;     // dst offset from the cell's origin, in cell units
} TL_Piece;

typedef struct {
  TL_Piece pieces[6]; // in-cell draw order; across cells, draw layer by layer
  U32 count;
} TL_Cell;

// neighborhood: the cell's 3x3 class window, row-major (index 4 = the cell
// itself). Class ids at-or-past class_count read as class 0 -- the caller's
// bad-id diagnostic path. p is the cell's world position.
internal TL_Cell tl_cell(TL_Config* config, U32 neighborhood[9], V2I p);
