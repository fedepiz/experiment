#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Draw Layer
//
// This layer puts a meaning and a resource on the quads of the render layer:
// a font, an image, a sprite sheet, and the vocabulary of an immediate mode
// drawing interface.
//
// Each function here becomes a set of calls to r_push_quad. The render layer
// stays private: no R_ type is in this interface, and draw.c is the one file
// that includes render.h.
//
// Each coordinate is in points, in the space of the window, from the top left
// corner. Each color is a V4 that holds RGBA from 0 to 1. Each call draws
// above the calls before it.

////////////////////////////////
//~ fp: Frame Lifecycle
//
// These functions hold the frame of the render layer. The application calls
// the draw layer, and the draw layer calls the render layer.

internal void d_init(void);
internal void d_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels);
internal void d_frame_end(void);

////////////////////////////////
//~ fp: Clip Stack
//
// A clip applies to each shape that you draw while it is on the stack. A
// second clip inside the first gives the intersection of the two. The clip of
// a quad in the render layer holds the result, so a clip cannot end a batch.

internal void d_push_clip(Rect r);
internal void d_pop_clip(void);

////////////////////////////////
//~ fp: Camera
//
// A camera is a value. The state of the game holds one and moves it. The same
// struct serves two uses that do not depend on each other: it makes the draws
// between begin and end read world coordinates, and it converts one point,
// which the code for input and for a UI that follows the world uses without
// any draw.
//
// There are two spaces: the world space, inside begin and end, and the screen
// space, which is the default. There is no stack of transforms, and a camera
// inside a camera is not possible.
//
// The CPU applies the transform at the push, so the render layer holds no
// camera, and a camera cannot end a batch. A clip is in screen space
// everywhere. To clip to an area of the world, convert the corners of that
// area and push the result.
//
// A camera does not turn for now. A turn would make each rect of the world
// unaligned to the axes, against the rules for a corner radius and a border.
//
// Text in world space changes size as the geometry does, so a zoom makes the
// glyphs larger and does not draw them again at the new size. A later change
// inside d_text can correct that, and the interface stays the same.

typedef struct {
  V2  center; // the point of the world at the center of the viewport
  F32 zoom;   // The points of the screen for each unit of the world. A zoom of
              // 0 reads as 1, so the zero camera is the identity camera at the
              // origin (ZII).

  // The state of the movement with inertia. The code that steers the camera
  // holds it. See camera_update. The transform above does not read it. A zero
  // camera stands still (ZII).
  V2  pan_vel;  // the units of the world for each second
  F32 zoom_vel; // the doublings for each second
} D_Camera;

// These two functions read the camera value and the size of the viewport, and
// do nothing more. The size comes from the current frame, so between two
// frames a conversion uses the frame that began last. A resize makes that size
// one frame old at worst.
internal V2 d_camera_to_screen(D_Camera cam, V2 world_point);
internal V2 d_camera_from_screen(D_Camera cam, V2 screen_point);

internal void d_camera_begin(D_Camera cam); // each draw between the two is in world space
internal void d_camera_end(void);           // go back to screen space

////////////////////////////////
//~ fp: Rectangles / Lines
//
// d_rect_ex gives control of each value. The functions below it cover the
// usual cases, and most call sites read better with one of those.

typedef struct {
  Rect rect;
  V4  colors[Corner_COUNT];       // A color at each corner makes a gradient.
  F32 corner_radii[Corner_COUNT];
  F32 border_thickness; // A value of 0 fills the rect.
  F32 edge_softness;    // A value of 0 gives a hard edge.
} D_RectParams;

internal void d_rect_ex(D_RectParams* params);
internal void d_rect(Rect r, V4 color);
internal void d_rect_rounded(Rect r, V4 color, F32 radius);
internal void d_rect_outline(Rect r, V4 color, F32 thickness);
internal void d_rect_gradient_v(Rect r, V4 top, V4 bottom); // a gradient from the top to the bottom

internal void d_line(V2 a, V2 b, F32 thickness, V4 color);

////////////////////////////////
//~ fp: Images (CPU) / Sprites & Sheets (GPU)
//
// A D_Image is a block of decoded pixels on an arena, and nothing more.
//
// An image goes to the GPU in a sprite sheet. The build of a sheet is a phase
// with a start and an end, between d_spritesheet_begin and d_spritesheet_end.
// Build a sheet at load time, and not inside a frame. Each image that you push
// in that phase goes into the sheet, and comes back as a D_Sprite.
//
// The draw layer names no texture. The sheet is the texture, and a sprite is
// the one resource that you can draw.
//
// The sheet packs each image as it arrives, on shelves. A sprite is therefore
// complete when the push gives it back, and no later step can change it. Push
// the tall images first for a smaller sheet. Each sheet samples linearly for
// now, and a font does too.

// The image holds RGBA8. The caller does not read a byte: the functions below
// read and write a pixel as a V4 color from 0 to 1.
typedef struct {
  I32 w;
  I32 h;
  U8* pixels; // RGBA8, which is w*h*4 bytes
} D_Image;

// A file that is absent, and a file that the decoder cannot read, both give
// the zero image (ZII). The decoder reads each format of stb_image: png, jpg,
// bmp, tga, gif and more.
internal D_Image d_image_load(Arena* arena, String8 path);
internal D_Image d_image_create(Arena* arena, I32 w, I32 h, V4 color);

internal V4   d_image_get_px(D_Image image, I32 x, I32 y);           // a position outside the image, and a nil image, give {0}
internal void d_image_set_px(D_Image image, I32 x, I32 y, V4 color); // a position outside the image, and a nil image, write nothing
internal void d_image_blit(D_Image dst, I32 x, I32 y, D_Image src);  // Copy src into dst at (x, y), and clip it. A nil image on either side copies nothing.

//- fp: sprite sheets

typedef struct {
  U64 u64; // the id of the sheet, which the draw layer alone reads
} D_SpriteSheet;

// A sprite is a value, and not a handle to a hidden object. It says which
// sheet holds it, where it is in that sheet, and how large it is.
// d_spritesheet_push gives a complete sprite, which no later step changes.
// `size` is the member that a caller reads. The other members address the
// texels.
typedef struct {
  D_SpriteSheet sheet;
  Rect src; // the texels inside the sheet
  V2 size;  // The texels. It is the size of src, for the convenience of the caller.
} D_Sprite;

// How the texels of a sheet sample on the screen. The sheet takes this value
// when it starts, and the value does not change.
typedef U32 D_Sampling;
enum {
  D_Sampling_Smooth = 0, // linear, which is the default at a value of 0
  D_Sampling_Pixel,      // nearest. Pixel art stays hard at each zoom.
  D_Sampling_COUNT,
};

// w and h are the texels of the sheet. The caller chooses that size, because
// the sheet packs each image as it arrives, and therefore allocates before the
// content comes. A size of 0 gives 2048 (ZII).
internal void          d_spritesheet_begin(I32 w, I32 h, D_Sampling sampling);
internal D_Sprite      d_spritesheet_push(D_Image image); // A zero image, and a sheet with no space, give a nil sprite (ZII).
internal D_SpriteSheet d_spritesheet_end(void);

// Push one rectangular window of a canvas that wraps on both axes. The
// coordinates are texels.
//
// d_spritesheet_push puts empty texels in the gutter around a sprite. This
// function puts the texels of the canvas that follow the window there, and
// wraps at each edge of the canvas. Two windows of one continuous texture
// therefore meet with no seam under linear sampling. Without those texels each
// joint becomes a thin line, and the joints together read as a grid.
//
// A window that covers the whole canvas gives one sprite that tiles against
// itself. A nil canvas, and an empty window, give a nil sprite (ZII).
internal D_Sprite      d_spritesheet_push_window(D_Image canvas, Rect window);
internal void          d_spritesheet_release(D_SpriteSheet sheet); // Each sprite of that sheet then draws nothing.

// A sheet of one image, in one call, at the size of that image. Use it for an
// image that stands alone, such as a background.
internal D_Sprite d_sprite_from_image(D_Image image);

//- fp: sprite drawing

typedef struct {
  D_Sprite sprite;
  Rect dst;      // points
  Rect src;      // The texels, in the space of the sprite. A rect of 0 gives the whole sprite.
  V4 tint;       // It multiplies the color of the sprite. White keeps that color.
  F32 rotation;  // radians, counter-clockwise, about the center of dst
  D_Sprite mask; // An alpha mask across dst. The alpha of the sprite multiplies
                 // by the alpha of the mask at each pixel. The mask must be in
                 // the sheet of the sprite. A nil mask masks nothing.
} D_SpriteParams;

internal void d_sprite_ex(D_SpriteParams* params);
internal void d_sprite(D_Sprite sprite, Rect dst, V4 tint);

// Draw a sprite through an alpha mask. The boundary between two terrains uses
// this function most. An assert tests that the mask is in the sheet of the
// sprite: a quad reads one texture, so a mask cannot end a batch.
internal void d_sprite_masked(D_Sprite sprite, D_Sprite mask, Rect dst, V4 tint);

////////////////////////////////
//~ fp: Fonts / Text
//
// A font is a handle into the font cache of the draw layer.
//
// A glyph goes into an internal sprite sheet when a call draws it the first
// time, at the size and the scale of the screen of that call. That sheet stays
// open, because the set of glyphs grows while the program runs, and a phase
// with a start and an end cannot hold it. A caller does not see that sheet.
//
// A position of text is the TOP LEFT corner of the box around that text, which
// suits a layout. Use d_font_metrics where the baseline is what you need. Each
// size is in points, as in "text of 14 points". Each string is UTF-8.

typedef struct {
  U64 u64;
} D_Font;

internal D_Font d_font_open(String8 path); // A file that is absent, and a file that the parser cannot read, give a nil font, which draws nothing (ZII).
internal void   d_font_close(D_Font font);

typedef struct {
  F32 ascent;       // from the top of the box to the baseline
  F32 descent;      // from the baseline to the bottom of the box
  F32 line_advance; // from one baseline to the next baseline
} D_FontMetrics;

internal D_FontMetrics d_font_metrics(D_Font font, F32 size);

// The size of the box around the text. A layout starts from this function.
internal V2 d_text_dim(D_Font font, F32 size, String8 text);

// It gives the x of the pen after the last glyph, so a second call can
// continue the line. Use that to draw a line in more than one color, or in
// more than one style.
internal F32 d_text(D_Font font, F32 size, V2 pos, V4 color, String8 text);
