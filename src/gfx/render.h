#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Render Layer
//
// The GPU vocabulary and nothing else: textures made of raw bytes, and
// batches of textured quads. No fonts, no files, no notion of "text" or
// "sprite" -- meaning lives above, in draw. The interface is
// backend-agnostic (no GL types leak through it); the implementation is
// uniformly OpenGL for now, and batching is its whole job: quads pushed
// between frame_begin/frame_end accumulate on the frame arena and flush as
// few instanced draw calls at frame_end. Callers never think about draw-call
// economics.
//
// Quads render in exactly the order they are pushed (painter's algorithm --
// alpha blending makes order semantic, so nothing is ever reordered).
// Batches are maximal runs of consecutive quads sharing a texture; only a
// texture change can break a run, since everything else rides per-instance.
//
// Units: quad geometry is in points, window space, top-left origin -- the
// same space the window layer speaks. The framebuffer is in pixels;
// r_frame_begin takes the pixel size and the points->pixels scale and bakes
// both into the projection, so retina never leaks past this line.

////////////////////////////////
//~ fp: Handles
//
// Opaque, ZII: nil is {0}, as for every type -- deliberately no _nil()
// constructor and no is_nil() predicate, so nil can never drift away from
// zero. Nil-check by comparing against a zero literal:
// r_handle_eq(h, (R_Handle){0}). A struct rather than a bare U64 so handle
// types don't cross-convert.

typedef struct {
  U64 u64;
} R_Handle;

internal B32 r_handle_eq(R_Handle a, R_Handle b);

////////////////////////////////
//~ fp: Textures

typedef U32 R_TexFormat;
enum {
  R_TexFormat_Nil = 0,
  R_TexFormat_R8,    // one byte per texel; glyph atlas coverage
  R_TexFormat_RGBA8, // four bytes per texel
  R_TexFormat_COUNT,
};

typedef U32 R_TexSampling;
enum {
  R_TexSampling_Linear = 0, // the zero-is-default choice
  R_TexSampling_Nearest,    // pixel art, integer-scaled sprites
  R_TexSampling_COUNT,
};

// opt_data may be 0: the texture is allocated uncleared and filled later via
// r_tex_update (how the glyph atlas grows). Sizes are texels.
internal R_Handle r_tex_alloc(I32 w, I32 h, R_TexFormat format, R_TexSampling sampling, void* opt_data);
internal void     r_tex_release(R_Handle tex);
internal void     r_tex_update(R_Handle tex, I32 x, I32 y, I32 w, I32 h, void* data);

////////////////////////////////
//~ fp: Quads
//
// The one primitive. Everything above -- rects, glyphs, sprites, lines,
// rounded panels -- is some configuration of this struct. Keep it dumb: any
// field that means something ("font", "ninepatch") belongs in draw, composed
// out of these.

typedef struct {
  Rect dst;      // where, in points / window space
  Rect src;      // which texels of tex; ignored when tex is nil
  Rect mask_src; // texels of tex whose alpha multiplies the quad's alpha,
                 // stretched over dst; zero rect = no mask. Rides the same
                 // texture as src, so masking never breaks a batch.
  R_Handle tex;  // nil = solid color (an internal 1x1 white texture)

  V4  colors[Corner_COUNT];       // per-corner RGBA, 0..1; same in all four = flat
  F32 corner_radii[Corner_COUNT]; // points; 0 = square
  F32 border_thickness;             // points; 0 = filled, else outline
  F32 edge_softness;                // points of fade at edges; 0 = hard, ~1 = antialiased

  F32 rotation; // radians, counter-clockwise, about the center of dst; for
                // textured sprites. Corner radii and borders assume an
                // axis-aligned quad and are ignored when rotation != 0.

  Rect clip; // points; pixels outside are dropped shader-side (no batch
             // break). Zero rect = no clipping.
} R_Quad;

////////////////////////////////
//~ fp: Frame Lifecycle

internal void r_init(void);

// frame_arena backs everything pushed this frame; the caller clears it after
// frame_end, per the usual per-frame arena discipline.
internal void r_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels);
internal void r_push_quad(R_Quad* quad);
internal void r_frame_end(void); // sort into batches, upload, draw
