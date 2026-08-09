#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Render Layer
//
// This layer holds the vocabulary of the GPU, and nothing more: a texture,
// which is a block of bytes, and a batch of quads with a texture. It holds no
// font, no file, and no idea of text or of a sprite. The draw layer above
// holds those.
//
// The interface names no type of the backend. The code below uses OpenGL for
// now.
//
// The work of this layer is the batch. Each quad that you push between
// r_frame_begin and r_frame_end goes on the frame arena. r_frame_end then
// draws the quads as a small number of instanced calls. The caller counts no
// draw calls.
//
// The renderer draws the quads in the order that the caller pushed them, and
// it moves no quad. Alpha blending gives that order a meaning. A batch is the
// longest run of quads that share one texture, so a change of texture is the
// one thing that ends a batch. Each other value of a quad goes with the
// instance.
//
// The geometry of a quad is in points, in the space of the window, from the
// top left corner. The window layer uses that space too. The framebuffer is in
// pixels. r_frame_begin takes the size in pixels and the scale from points to
// pixels, and puts both into the projection. No code above this line reads the
// size of a pixel.

////////////////////////////////
//~ fp: Handles
//
// The renderer alone reads the value of a handle. The shape of a handle is
// public: the nil handle is {0}, as with each other type, and `h.u64 == 0` is
// the test for it. A handle is a struct and not a U64, so two kinds of handle
// cannot convert into each other.

typedef struct {
  U64 u64;
} R_Handle;

////////////////////////////////
//~ fp: Textures

typedef U32 R_TexFormat;
enum {
  R_TexFormat_Nil = 0,
  R_TexFormat_R8,    // one byte for each texel. The glyph atlas uses it.
  R_TexFormat_RGBA8, // four bytes for each texel
  R_TexFormat_COUNT,
};

typedef U32 R_TexSampling;
enum {
  R_TexSampling_Linear = 0, // the default, which is the value 0
  R_TexSampling_Nearest,    // pixel art, and a sprite at an integer scale
  R_TexSampling_COUNT,
};

// opt_data can be 0. The texture then holds the bytes that the memory held,
// and a later call to r_tex_update writes it. The glyph atlas grows in that
// way. Each size is in texels.
internal R_Handle r_tex_alloc(I32 w, I32 h, R_TexFormat format, R_TexSampling sampling, void* opt_data);
internal void     r_tex_release(R_Handle tex);
internal void     r_tex_update(R_Handle tex, I32 x, I32 y, I32 w, I32 h, void* data);

////////////////////////////////
//~ fp: Quads
//
// The quad is the one primitive. Each shape above this layer is a set of
// values of this struct: a rectangle, a glyph, a sprite, a line and a panel
// with round corners. This struct holds no member that carries a meaning. A
// member such as a font or a ninepatch belongs to the draw layer, which builds
// it from these quads.

typedef struct {
  Rect dst;      // the place, in points, in the space of the window
  Rect src;      // the texels of tex. A nil tex makes this member unused.
  Rect mask_src; // The texels of tex whose alpha multiplies the alpha of the
                 // quad, across dst. A rect of 0 gives no mask. The mask is on
                 // the texture of src, so a mask cannot end a batch.
  R_Handle tex;  // A nil handle gives a solid color. The renderer then uses an
                 // internal white texture of 1x1 texels.

  V4  colors[Corner_COUNT];       // the RGBA of each corner, from 0 to 1. Four equal colors give a flat quad.
  F32 corner_radii[Corner_COUNT]; // points. A radius of 0 gives a square corner.
  F32 border_thickness;           // points. A value of 0 fills the quad. A larger value draws an outline.
  F32 edge_softness;              // the points of fade at an edge. 0 is hard, and about 1 is smooth.

  F32 rotation; // Radians, counter-clockwise, about the center of dst. A
                // corner radius and a border are in the local space of the
                // quad, so they turn with the quad.

  Rect clip; // Points. The shader removes each pixel outside this rect, so a
             // clip cannot end a batch. A rect of 0 clips nothing.
} R_Quad;

////////////////////////////////
//~ fp: Frame Lifecycle

internal void r_init(void);

// frame_arena holds each quad of this frame. The caller clears that arena
// after r_frame_end, as it clears each arena of a frame.
internal void r_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels); // the scale must be above 0. The caller chooses a value for a ZII default.
internal void r_push_quad(R_Quad* quad);
internal void r_frame_end(void); // send the batches of r_push_quad to the GPU, and draw them
