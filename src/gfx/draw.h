#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Draw Layer
//
// Meaning and resources on top of render's dumb quads: fonts, images and
// sprite sheets, and an immediate-mode drawing vocabulary. Everything here
// compiles down to r_push_quad -- but render is an implementation detail: no
// R_ type appears in this interface, and draw.c is the only file that
// includes render.h.
//
// All coordinates are points, window space, top-left origin. Colors are V4
// RGBA in 0..1. Draw calls take effect in call order (painter's algorithm).

////////////////////////////////
//~ fp: Frame Lifecycle
//
// Wraps render's frame: the app talks to draw, draw drives render.

internal void d_init(void);
internal void d_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels);
internal void d_frame_end(void);

////////////////////////////////
//~ fp: Clip Stack
//
// Applied to everything drawn while pushed; nests by intersection. Backed by
// render's per-quad clip, so clipping never breaks a batch.

internal void d_push_clip(Rect r);
internal void d_pop_clip(void);

////////////////////////////////
//~ fp: Camera
//
// A camera is a plain value: your game state owns one, tweens it, and the
// same struct serves two independent uses -- scoping world-space drawing,
// and pure point conversion (input picking, world-anchored UI) that never
// touches the drawing machinery. There are exactly two spaces: world (inside
// begin/end) and screen (the default); no transform stack, no nesting.
//
// The transform is applied CPU-side at push, so render never learns cameras
// exist and camera scopes never break batches. Clips stay screen-space
// everywhere; to clip to a world region, convert its corners and push that.
//
// No camera rotation for now: it would make every world rect non-axis-
// aligned, colliding with the corner-radius/border contract. World-space
// text scales like geometry (magnifies under zoom rather than
// re-rasterizing) -- fixable inside d_text later, no API impact.

typedef struct {
  V2  center; // world point at the viewport center
  F32 zoom;   // screen points per world unit; 0 reads as 1, so the zero
              // camera is a valid identity camera at the origin (ZII)

  // inertial movement state, carried for whoever steers the camera (see
  // camera_update); the transform math below never reads it. ZII: the zero
  // camera is at rest.
  V2  pan_vel;  // world units per second
  F32 zoom_vel; // doublings per second
} D_Camera;

// Pure math on the camera value, except that the viewport size is read from
// the current frame -- so between frames, conversions are relative to the
// last-begun frame (at worst one resize stale).
internal V2 d_camera_to_screen(D_Camera cam, V2 world_point);
internal V2 d_camera_from_screen(D_Camera cam, V2 screen_point);

internal void d_camera_begin(D_Camera cam); // draws inside are world-space
internal void d_camera_end(void);           // back to screen-space

////////////////////////////////
//~ fp: Rectangles / Lines
//
// d_rect_ex is the full-control path; the wrappers below cover the common
// cases and are what most call sites should read as.

typedef struct {
  Rect rect;
  V4  colors[Corner_COUNT];       // gradients via per-corner colors
  F32 corner_radii[Corner_COUNT];
  F32 border_thickness; // 0 = filled
  F32 edge_softness;    // 0 = hard edge
} D_RectParams;

internal void d_rect_ex(D_RectParams* params);
internal void d_rect(Rect r, V4 color);
internal void d_rect_rounded(Rect r, V4 color, F32 radius);
internal void d_rect_outline(Rect r, V4 color, F32 thickness);
internal void d_rect_gradient_v(Rect r, V4 top, V4 bottom); // vertical gradient

internal void d_line(V2 a, V2 b, F32 thickness, V4 color);

////////////////////////////////
//~ fp: Images (CPU) / Sprites & Sheets (GPU)
//
// A D_Image is decoded pixels on an arena -- bytes, nothing more. GPU
// residency happens by building sprite sheets: an explicit, bounded phase
// (load time, not mid-frame) between spritesheet_begin/end, during which
// each pushed image is packed into the sheet and comes back as a D_Sprite.
// Draw never talks of textures: the sheet IS the texture, and the only
// drawable resource is a sprite.
//
// Packing is incremental (shelf-based), which is what lets a sprite be final
// the moment push returns -- no fishing results out of a deferred pack.
// Push tall images first for tighter sheets, if you care. Everything samples
// linearly for now, fonts included.

// Storage is RGBA8, but callers never frob bytes: pixels read and write as
// V4 colors in 0..1 through the accessors below.
typedef struct {
  I32 w;
  I32 h;
  U8* pixels; // RGBA8, w*h*4 bytes
} D_Image;

// ZII: a missing or undecodable file loads as the zero image. Decodes
// whatever stb_image speaks (png, jpg, bmp, tga, gif, ...).
internal D_Image d_image_load(Arena* arena, String8 path);
internal D_Image d_image_create(Arena* arena, I32 w, I32 h, V4 color);

internal V4   d_image_get_px(D_Image image, I32 x, I32 y);           // out of bounds / nil image: {0}
internal void d_image_set_px(D_Image image, I32 x, I32 y, V4 color); // out of bounds / nil image: no-op
internal void d_image_blit(D_Image dst, I32 x, I32 y, D_Image src);  // copy src into dst at (x, y), clipped; nil either: no-op

//- fp: sprite sheets

typedef struct {
  U64 u64; // draw-internal sheet id
} D_SpriteSheet;

// A plain value, not a handle into hidden machinery: which sheet, where in
// it, how big -- final from the moment spritesheet_push returns. size is the
// field callers are expected to read; the rest is addressing.
typedef struct {
  D_SpriteSheet sheet;
  Rect src; // texels within the sheet
  V2 size;  // texels; == src dims, for convenience
} D_Sprite;

// w/h are the sheet's texel dimensions -- capacity is the caller's call,
// since eager packing must allocate before the content arrives. 0 reads as
// a 2048 default (ZII).
internal void          d_spritesheet_begin(I32 w, I32 h);
internal D_Sprite      d_spritesheet_push(D_Image image); // ZII: zero image or full sheet = nil sprite
internal D_SpriteSheet d_spritesheet_end(void);

// pushes one rectangular window of a toroidally-wrapping canvas (texel
// coords). Unlike d_spritesheet_push, the sprite's gutter ring is filled
// with the canvas texels just past the window -- wrapping at the canvas
// edges -- so windows of one continuous texture butt seamlessly under
// linear sampling instead of hardening every joint into a faint grid.
// A window covering the whole canvas yields a single self-tiling sprite.
// ZII: a nil canvas or empty window = nil sprite.
internal D_Sprite d_spritesheet_push_window(D_Image canvas, Rect window);
internal void          d_spritesheet_release(D_SpriteSheet sheet); // its sprites draw nothing afterwards

// a one-image sheet in one call, sized exactly to the image, for standalone
// images (backgrounds, ...)
internal D_Sprite d_sprite_from_image(D_Image image);

//- fp: sprite drawing

typedef struct {
  D_Sprite sprite;
  Rect dst;      // points
  Rect src;      // texels, in the sprite's own space; zero rect = whole sprite
  V4 tint;       // multiplies sprite color; white = as-is
  F32 rotation;  // radians, counter-clockwise, about the center of dst
  D_Sprite mask; // alpha mask stretched over dst: the sprite's alpha
                 // multiplies by the mask's alpha at each pixel. Must live in
                 // the same sheet as sprite; nil = unmasked.
} D_SpriteParams;

internal void d_sprite_ex(D_SpriteParams* params);
internal void d_sprite(D_Sprite sprite, Rect dst, V4 tint);

// sprite drawn through an alpha mask; the workhorse of terrain boundary
// spill. Precondition (asserted): mask lives in the same sheet as sprite --
// one texture per quad, so masking never breaks a render batch.
internal void d_sprite_masked(D_Sprite sprite, D_Sprite mask, Rect dst, V4 tint);

////////////////////////////////
//~ fp: Fonts / Text
//
// Fonts are handles into draw's font cache. Glyphs rasterize lazily into an
// internal, always-open sprite sheet at the size (and retina scale) they are
// first drawn at -- the one case where the content is dynamic, so the sheet
// cannot be built in a bounded begin/end phase. Callers never see it.
//
// Text positions are the TOP-LEFT of the text's bounding box -- the layout-
// friendly convention; use d_font_metrics for baseline math when it matters.
// Sizes are in points ("14pt text"), and text is assumed UTF-8.

typedef struct {
  U64 u64;
} D_Font;

internal D_Font d_font_open(String8 path); // ZII: missing/broken file = nil font, draws nothing
internal void   d_font_close(D_Font font);

typedef struct {
  F32 ascent;      // top of box -> baseline
  F32 descent;     // baseline -> bottom of box
  F32 line_advance; // baseline -> next baseline
} D_FontMetrics;

internal D_FontMetrics d_font_metrics(D_Font font, F32 size);

// dimensions of the text's bounding box; the layout building block
internal V2 d_text_dim(D_Font font, F32 size, String8 text);

// returns the pen x after the last glyph, so runs can be chained
// (multi-color spans, inline styling)
internal F32 d_text(D_Font font, F32 size, V2 pos, V4 color, String8 text);
