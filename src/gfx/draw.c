#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/os.h"
#include "gfx/render.h"
#include "gfx/draw.h"

#include <math.h>

////////////////////////////////
//~ fp: Draw Layer Implementation
//
// This is the one file that calls the render layer.
//
// The sheet holds each resource. A sprite sheet packs its images on shelves,
// as each image arrives, so a position is complete at the push, which is what
// the interface promises. The glyph atlas is one more sheet, and it never
// closes.
//
// Each shape goes through d__emit_screen, which puts the current clip on the
// quad. d__emit is the entry for a rect, and it applies the camera transform
// first. d_line does not use d__emit, because the quad of a line exists only
// after the transform of its two end points.

// stbtt and stbi call malloc for their temporary memory. That memory stays
// inside those two libraries. Each allocation of this program is on an arena,
// as usual.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "gfx/stb_truetype.h"
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO // A file arrives as bytes, from os_read_entire_file.
#define STB_IMAGE_IMPLEMENTATION
#include "gfx/stb_image.h"
#pragma clang diagnostic pop

////////////////////////////////
//~ fp: State

#define D_SHEET_CAP         64
#define D_FONT_CAP          16
#define D_CLIP_STACK_CAP    64
#define D_SHEET_DEFAULT_DIM 2048
#define D_GLYPH_BUCKETS     256

typedef struct {
  B32 used;
  R_Handle tex;
  I32 w;
  I32 h;
  //- the packer of the shelves
  I32 shelf_x;
  I32 shelf_y;
  I32 shelf_h;
} D_Sheet;

typedef struct D_Glyph D_Glyph;
struct D_Glyph {
  D_Glyph* next;
  U32 codepoint;
  I32 size_px;    // The height in pixels of the raster. It is part of the key of the cache.
  B32 has_bitmap; // A space moves the pen and draws nothing.
  Rect src;       // the texels in the glyph sheet
  F32 w_px, h_px;
  F32 xoff_px, yoff_px; // the offset of the bitmap from the pen, in pixels
  F32 advance_px;
};

typedef struct {
  B32 used;
  String8 ttf;
  stbtt_fontinfo info;
  D_Glyph* glyphs[D_GLYPH_BUCKETS];
} D_FontSlot;

typedef struct {
  B32 initialized;
  Arena* arena; // It holds the font files and the glyph nodes, for the whole program.

  //- the members of one frame
  Arena* frame_arena;
  F32 scale;
  V2 viewport_pts;

  //- The stack of clips. Each entry is the intersection with the entry below
  //  it, so a read needs no further work.
  Rect clip_stack[D_CLIP_STACK_CAP];
  U64 clip_count;

  //- the camera
  D_Camera camera;
  B32 camera_active;

  //- the sheets
  D_Sheet sheets[D_SHEET_CAP];
  U64 open_sheet;  // The slot plus 1 of the sheet that the code builds now. 0 says that there is none.
  U64 glyph_sheet; // The slot plus 1. The first raster of a glyph makes this sheet.

  //- the fonts
  D_FontSlot fonts[D_FONT_CAP];
} D_State;

global D_State d_state;

////////////////////////////////
//~ fp: Frame Lifecycle

internal void d_init(void) {
  r_init();
  d_state.arena = arena_alloc();
  d_state.initialized = 1;
}

internal void d_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels) {
  d_state.frame_arena = frame_arena;
  d_state.scale = (points_to_pixels == 0) ? 1.0f : points_to_pixels; // A scale of 0 reads as 1 (ZII).
  d_state.viewport_pts.x = framebuffer_size_px.x / d_state.scale;
  d_state.viewport_pts.y = framebuffer_size_px.y / d_state.scale;
  d_state.clip_count = 0;
  d_state.camera_active = 0;
  r_frame_begin(frame_arena, framebuffer_size_px, d_state.scale);
}

internal void d_frame_end(void) {
  Assert(d_state.clip_count == 0); // a d_push_clip with no d_pop_clip
  Assert(!d_state.camera_active);  // a d_camera_begin with no d_camera_end
  r_frame_end();
  d_state.frame_arena = 0;
}

////////////////////////////////
//~ fp: Camera + Quad Emission
//
// Each shape goes through these functions. d__emit applies the camera
// transform, which converts world coordinates to screen coordinates. Both
// functions then apply the current clip, and call the render layer.

internal F32 d_camera__zoom(D_Camera cam) {
  return (cam.zoom == 0) ? 1.0f : cam.zoom; // The zero camera is the identity camera (ZII).
}

internal V2 d_camera_to_screen(D_Camera cam, V2 world_point) {
  F32 zoom = d_camera__zoom(cam);
  V2 result;
  result.x = (world_point.x - cam.center.x) * zoom + 0.5f * d_state.viewport_pts.x;
  result.y = (world_point.y - cam.center.y) * zoom + 0.5f * d_state.viewport_pts.y;
  return result;
}

internal V2 d_camera_from_screen(D_Camera cam, V2 screen_point) {
  F32 zoom = d_camera__zoom(cam);
  V2 result;
  result.x = (screen_point.x - 0.5f * d_state.viewport_pts.x) / zoom + cam.center.x;
  result.y = (screen_point.y - 0.5f * d_state.viewport_pts.y) / zoom + cam.center.y;
  return result;
}

internal void d_camera_begin(D_Camera cam) {
  d_state.camera = cam;
  d_state.camera_active = 1;
}

internal void d_camera_end(void) {
  d_state.camera_active = 0;
}

// A quad in screen space. This function applies the clip and nothing more.
internal void d__emit_screen(R_Quad* quad) {
  if(d_state.clip_count > 0) {
    quad->clip = d_state.clip_stack[d_state.clip_count - 1];
  }
  r_push_quad(quad);
}

// A quad that can be in world space. This function applies the camera
// transform first. The geometry changes size with the zoom. A rotation does
// not change with a scale, so it passes through.
internal void d__emit(R_Quad* quad) {
  if(d_state.camera_active) {
    F32 zoom = d_camera__zoom(d_state.camera);
    quad->dst.min = d_camera_to_screen(d_state.camera, quad->dst.min);
    quad->dst.max = d_camera_to_screen(d_state.camera, quad->dst.max);
    for(U32 corner = 0; corner < Corner_COUNT; corner += 1) {
      quad->corner_radii[corner] *= zoom;
    }
    quad->border_thickness *= zoom;
    quad->edge_softness *= zoom;
  }
  d__emit_screen(quad);
}

////////////////////////////////
//~ fp: Clip Stack

internal void d_push_clip(Rect r) {
  if(d_state.clip_count < D_CLIP_STACK_CAP) {
    Rect resolved = r;
    if(d_state.clip_count > 0) {
      Rect top = d_state.clip_stack[d_state.clip_count - 1];
      resolved.min.x = Max(resolved.min.x, top.min.x);
      resolved.min.y = Max(resolved.min.y, top.min.y);
      resolved.max.x = Min(resolved.max.x, top.max.x);
      resolved.max.y = Min(resolved.max.y, top.max.y);
    }
    if(resolved.max.x <= resolved.min.x || resolved.max.y <= resolved.min.y) {
      // Two clips that do not overlap remove each fragment. A rect of 0 reads
      // as "no clip" further down, so put this rect where nothing draws.
      resolved = (Rect){{-1e8f, -1e8f}, {-1e8f + 1.0f, -1e8f + 1.0f}};
    }
    d_state.clip_stack[d_state.clip_count] = resolved;
    d_state.clip_count += 1;
  }
}

internal void d_pop_clip(void) {
  if(d_state.clip_count > 0) {
    d_state.clip_count -= 1;
  }
}

////////////////////////////////
//~ fp: Rectangles / Lines

internal void d_rect_ex(D_RectParams* params) {
  R_Quad quad = {0};
  quad.dst = params->rect;
  for(U32 corner = 0; corner < Corner_COUNT; corner += 1) {
    quad.colors[corner] = params->colors[corner];
    quad.corner_radii[corner] = params->corner_radii[corner];
  }
  quad.border_thickness = params->border_thickness;
  quad.edge_softness = params->edge_softness;
  d__emit(&quad);
}

internal void d_rect(Rect r, V4 color) {
  D_RectParams params = {0};
  params.rect = r;
  for(U32 corner = 0; corner < Corner_COUNT; corner += 1) { params.colors[corner] = color; }
  d_rect_ex(&params);
}

internal void d_rect_rounded(Rect r, V4 color, F32 radius) {
  D_RectParams params = {0};
  params.rect = r;
  for(U32 corner = 0; corner < Corner_COUNT; corner += 1) {
    params.colors[corner] = color;
    params.corner_radii[corner] = radius;
  }
  params.edge_softness = 1.0f; // A curve needs a smooth edge.
  d_rect_ex(&params);
}

internal void d_rect_outline(Rect r, V4 color, F32 thickness) {
  D_RectParams params = {0};
  params.rect = r;
  for(U32 corner = 0; corner < Corner_COUNT; corner += 1) { params.colors[corner] = color; }
  params.border_thickness = thickness;
  params.edge_softness = 1.0f;
  d_rect_ex(&params);
}

internal void d_rect_gradient_v(Rect r, V4 top, V4 bottom) {
  D_RectParams params = {0};
  params.rect = r;
  params.colors[Corner_TL] = params.colors[Corner_TR] = top;
  params.colors[Corner_BL] = params.colors[Corner_BR] = bottom;
  d_rect_ex(&params);
}

internal void d_line(V2 a, V2 b, F32 thickness, V4 color) {
  // The two end points convert as points. The code then builds the quad in
  // screen space.
  F32 zoom = 1.0f;
  if(d_state.camera_active) {
    zoom = d_camera__zoom(d_state.camera);
    a = d_camera_to_screen(d_state.camera, a);
    b = d_camera_to_screen(d_state.camera, b);
  }
  F32 dx = b.x - a.x;
  F32 dy = b.y - a.y;
  F32 len = sqrtf(dx * dx + dy * dy);
  if(len > 0) {
    F32 half_len = 0.5f * len;
    F32 half_thick = 0.5f * thickness * zoom;
    V2 center = {0.5f * (a.x + b.x), 0.5f * (a.y + b.y)};
    R_Quad quad = {0};
    quad.dst = (Rect){{center.x - half_len, center.y - half_thick},
                      {center.x + half_len, center.y + half_thick}};
    for(U32 corner = 0; corner < Corner_COUNT; corner += 1) { quad.colors[corner] = color; }
    // The rotation of a quad is counter-clockwise for a person who looks at
    // the screen. y grows toward the bottom, so the angle takes a minus sign.
    quad.rotation = atan2f(-dy, dx);
    quad.edge_softness = 1.0f;
    d__emit_screen(&quad);
  }
}

////////////////////////////////
//~ fp: Sheets

internal D_Sheet* d__sheet_from_id(U64 id) {
  D_Sheet* result = 0;
  if(id != 0 && id <= D_SHEET_CAP && d_state.sheets[id - 1].used) {
    result = &d_state.sheets[id - 1];
  }
  return result;
}

// Take a slot for a sheet, and make the texture of that sheet on the GPU.
// opt_data writes the texture as it is, which suits a sheet of one image at
// the size of that image. With no data the texture holds zeros. A pusher that
// leaves the gutter ring empty, such as the pusher of a glyph, needs a
// transparent black there, and not the bytes that the memory held.
internal U64 d__sheet_create(I32 w, I32 h, R_TexFormat format, D_Sampling sampling, void* opt_data) {
  U64 result = 0;
  U64 slot = D_SHEET_CAP;
  for(U64 i = 0; i < D_SHEET_CAP; i += 1) {
    if(!d_state.sheets[i].used) {
      slot = i;
      break;
    }
  }
  if(slot < D_SHEET_CAP && w > 0 && h > 0) {
    R_TexSampling tex_sampling = (sampling == D_Sampling_Pixel) ? R_TexSampling_Nearest
                                                                : R_TexSampling_Linear;
    R_Handle tex = {0};
    if(opt_data != 0) {
      tex = r_tex_alloc(w, h, format, tex_sampling, opt_data);
    } else {
      U64 texel_size = (format == R_TexFormat_R8) ? 1 : 4;
      ArenaTemp scratch = arena_get_scratch(0, 0);
      U8* zeroes = push_array(scratch.arena, U8, (U64)w * (U64)h * texel_size);
      tex = r_tex_alloc(w, h, format, tex_sampling, zeroes);
      arena_release_scratch(scratch);
    }
    if(tex.u64 != 0) {
      D_Sheet* sheet = &d_state.sheets[slot];
      MemoryZeroStruct(sheet);
      sheet->used = 1;
      sheet->tex = tex;
      sheet->w = w;
      sheet->h = h;
      result = slot + 1;
    }
  }
  return result;
}

// The packer puts each image on a shelf as that image arrives. The position
// is complete when the packer gives it back, which is what makes a sprite
// complete at the push.
//
// Each region takes a ring of D__SHEET_GUTTER texels around its content, so
// linear sampling at the edge of a sprite cannot reach the region next to it.
//
// The pusher decides what the ring holds. An image copies its edge texels into
// the ring, so two tiles meet with no seam. A glyph leaves the ring
// transparent black, which is correct for text with an alpha blend.
#define D__SHEET_GUTTER 1

internal B32 d__sheet_pack(D_Sheet* sheet, I32 w, I32 h, I32* out_x, I32* out_y) {
  B32 result = 0;
  I32 fw = w + 2 * D__SHEET_GUTTER;
  I32 fh = h + 2 * D__SHEET_GUTTER;
  if(sheet->shelf_x + fw > sheet->w) { // The shelf has no space. Start the next shelf.
    sheet->shelf_y += sheet->shelf_h;
    sheet->shelf_x = 0;
    sheet->shelf_h = 0;
  }
  if(sheet->shelf_x + fw <= sheet->w && sheet->shelf_y + fh <= sheet->h) {
    *out_x = sheet->shelf_x + D__SHEET_GUTTER;
    *out_y = sheet->shelf_y + D__SHEET_GUTTER;
    sheet->shelf_x += fw;
    sheet->shelf_h = Max(sheet->shelf_h, fh);
    result = 1;
  }
  return result;
}

internal void d_spritesheet_begin(I32 w, I32 h, D_Sampling sampling) {
  Assert(d_state.open_sheet == 0);        // the code can build one sheet at a time
  if(w == 0) { w = D_SHEET_DEFAULT_DIM; } // A size of 0 reads as the default (ZII).
  if(h == 0) { h = D_SHEET_DEFAULT_DIM; }
  d_state.open_sheet = d__sheet_create(w, h, R_TexFormat_RGBA8, sampling, 0);
}

internal D_Sprite d_spritesheet_push(D_Image image) {
  D_Sprite result = {0};
  D_Sheet* sheet = d__sheet_from_id(d_state.open_sheet);
  if(sheet != 0 && image.pixels != 0 && image.w > 0 && image.h > 0) {
    I32 x = 0, y = 0;
    if(d__sheet_pack(sheet, image.w, image.h, &x, &y)) {
      // Send the image and its gutter ring. The ring holds a copy of the edge
      // texels of the image. Linear sampling at the border of the sprite then
      // mixes the colors of that sprite alone, so two tiles meet with no
      // seam.
      ArenaTemp scratch = arena_get_scratch(0, 0);
      I32 fw = image.w + 2 * D__SHEET_GUTTER;
      I32 fh = image.h + 2 * D__SHEET_GUTTER;
      U8* staging = push_array_no_zero(scratch.arena, U8, (U64)fw * fh * 4);
      for(I32 sy = 0; sy < fh; sy += 1) {
        I32 cy = Clamp(0, sy - D__SHEET_GUTTER, image.h - 1);
        for(I32 sx = 0; sx < fw; sx += 1) {
          I32 cx = Clamp(0, sx - D__SHEET_GUTTER, image.w - 1);
          MemoryCopy(staging + ((U64)sy * fw + sx) * 4,
                     image.pixels + ((U64)cy * image.w + cx) * 4, 4);
        }
      }
      r_tex_update(sheet->tex, x - D__SHEET_GUTTER, y - D__SHEET_GUTTER, fw, fh, staging);
      arena_release_scratch(scratch);

      result.sheet.u64 = d_state.open_sheet;
      result.src = (Rect){{(F32)x, (F32)y}, {(F32)(x + image.w), (F32)(y + image.h)}};
      result.size = (V2){(F32)image.w, (F32)image.h};
    }
    // The sheet has no space. The function then gives the nil sprite (ZII).
  }
  return result;
}

internal D_Sprite d_spritesheet_push_window(D_Image canvas, Rect window) {
  D_Sprite result = {0};
  D_Sheet* sheet = d__sheet_from_id(d_state.open_sheet);
  I32 x0 = (I32)window.min.x;
  I32 y0 = (I32)window.min.y;
  I32 w = (I32)window.max.x - x0;
  I32 h = (I32)window.max.y - y0;
  if(sheet != 0 && canvas.pixels != 0 && w > 0 && h > 0) {
    I32 x = 0, y = 0;
    if(d__sheet_pack(sheet, w, h, &x, &y)) {
      // Send the window and its gutter ring. The ring holds the texels of the
      // canvas that follow the window, and it wraps at each edge of the
      // canvas. Linear sampling at the border of the sprite then mixes into
      // the true neighbour of the window, so two parts of one texture that
      // wraps meet with no seam on the screen. A ring of copied edge texels,
      // which d_spritesheet_push makes, would give each joint a thin line, and
      // those lines together read as a grid. A window that covers the whole
      // canvas gives one sprite that tiles against itself.
      ArenaTemp scratch = arena_get_scratch(0, 0);
      I32 fw = w + 2 * D__SHEET_GUTTER;
      I32 fh = h + 2 * D__SHEET_GUTTER;
      U8* staging = push_array_no_zero(scratch.arena, U8, (U64)fw * fh * 4);
      for(I32 sy = 0; sy < fh; sy += 1) {
        I32 cy = (y0 + sy - D__SHEET_GUTTER + canvas.h) % canvas.h;
        for(I32 sx = 0; sx < fw; sx += 1) {
          I32 cx = (x0 + sx - D__SHEET_GUTTER + canvas.w) % canvas.w;
          MemoryCopy(staging + ((U64)sy * fw + sx) * 4,
                     canvas.pixels + ((U64)cy * canvas.w + cx) * 4, 4);
        }
      }
      r_tex_update(sheet->tex, x - D__SHEET_GUTTER, y - D__SHEET_GUTTER, fw, fh, staging);
      arena_release_scratch(scratch);

      result.sheet.u64 = d_state.open_sheet;
      result.src = (Rect){{(F32)x, (F32)y}, {(F32)(x + w), (F32)(y + h)}};
      result.size = (V2){(F32)w, (F32)h};
    }
  }
  return result;
}

internal D_SpriteSheet d_spritesheet_end(void) {
  D_SpriteSheet result = {0};
  D_Sheet* sheet = d__sheet_from_id(d_state.open_sheet);
  if(sheet != 0) {
    result.u64 = d_state.open_sheet;
  }
  d_state.open_sheet = 0;
  return result;
}

internal void d_spritesheet_release(D_SpriteSheet id) {
  D_Sheet* sheet = d__sheet_from_id(id.u64);
  if(sheet != 0) {
    r_tex_release(sheet->tex);
    MemoryZeroStruct(sheet);
  }
}

internal D_Sprite d_sprite_from_image(D_Image image) {
  D_Sprite result = {0};
  if(image.pixels != 0 && image.w > 0 && image.h > 0) {
    // A sheet at the size of this image. There is no packing, and the texture
    // is the sprite.
    U64 id = d__sheet_create(image.w, image.h, R_TexFormat_RGBA8, D_Sampling_Smooth, image.pixels);
    if(id != 0) {
      result.sheet.u64 = id;
      result.src = (Rect){{0, 0}, {(F32)image.w, (F32)image.h}};
      result.size = (V2){(F32)image.w, (F32)image.h};
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Images

internal U8 d__u8_from_unit(F32 x) {
  F32 clamped = Clamp(0.0f, x, 1.0f);
  return (U8)(clamped * 255.0f + 0.5f);
}

internal D_Image d_image_load(Arena* arena, String8 path) {
  D_Image result = {0};
  B32 read_ok = 0;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  String8 file = os_read_entire_file(scratch.arena, path, &read_ok);
  if(read_ok && file.size > 0) {
    int w = 0, h = 0, source_comp = 0;
    U8* decoded = stbi_load_from_memory(file.str, (int)file.size, &w, &h, &source_comp, 4);
    if(decoded != 0) {
      result.w = (I32)w;
      result.h = (I32)h;
      result.pixels = push_array_no_zero(arena, U8, (U64)w * (U64)h * 4);
      MemoryCopy(result.pixels, decoded, (U64)w * (U64)h * 4);
      stbi_image_free(decoded); // stbi allocated it with malloc. The copy is on the arena.
    }
  }
  arena_release_scratch(scratch);
  return result;
}

internal D_Image d_image_create(Arena* arena, I32 w, I32 h, V4 color) {
  D_Image result = {0};
  if(w > 0 && h > 0) {
    result.w = w;
    result.h = h;
    result.pixels = push_array_no_zero(arena, U8, (U64)w * (U64)h * 4);
    U8 rgba[4] = {
      d__u8_from_unit(color.x),
      d__u8_from_unit(color.y),
      d__u8_from_unit(color.z),
      d__u8_from_unit(color.w),
    };
    U64 count = (U64)w * (U64)h;
    for(U64 i = 0; i < count; i += 1) {
      MemoryCopy(result.pixels + i * 4, rgba, 4);
    }
  }
  return result;
}

internal V4 d_image_get_px(D_Image image, I32 x, I32 y) {
  V4 result = {0};
  if(image.pixels != 0 && 0 <= x && x < image.w && 0 <= y && y < image.h) {
    U8* px = &image.pixels[((U64)y * (U64)image.w + (U64)x) * 4];
    result.x = (F32)px[0] / 255.0f;
    result.y = (F32)px[1] / 255.0f;
    result.z = (F32)px[2] / 255.0f;
    result.w = (F32)px[3] / 255.0f;
  }
  return result;
}

internal void d_image_set_px(D_Image image, I32 x, I32 y, V4 color) {
  if(image.pixels != 0 && 0 <= x && x < image.w && 0 <= y && y < image.h) {
    U8* px = &image.pixels[((U64)y * (U64)image.w + (U64)x) * 4];
    px[0] = d__u8_from_unit(color.x);
    px[1] = d__u8_from_unit(color.y);
    px[2] = d__u8_from_unit(color.z);
    px[3] = d__u8_from_unit(color.w);
  }
}

internal void d_image_blit(D_Image dst, I32 x, I32 y, D_Image src) {
  if(dst.pixels != 0 && src.pixels != 0) {
    // Clip src against dst. Each row that stays copies in full.
    I32 x0 = Max(x, 0);
    I32 y0 = Max(y, 0);
    I32 x1 = Min(x + src.w, dst.w);
    I32 y1 = Min(y + src.h, dst.h);
    for(I32 dy = y0; dy < y1 && x0 < x1; dy += 1) {
      MemoryCopy(dst.pixels + ((U64)dy * dst.w + x0) * 4,
                 src.pixels + ((U64)(dy - y) * src.w + (x0 - x)) * 4,
                 (U64)(x1 - x0) * 4);
    }
  }
}

////////////////////////////////
//~ fp: Sprite Drawing

internal void d_sprite_ex(D_SpriteParams* params) {
  D_Sheet* sheet = d__sheet_from_id(params->sprite.sheet.u64);
  if(sheet != 0) { // A nil sprite, and a sheet that is gone, draw nothing.
    R_Quad quad = {0};
    quad.dst = params->dst;
    quad.tex = sheet->tex;
    quad.rotation = params->rotation;

    // src is in the texel space of the sprite. A rect of 0 gives the whole
    // sprite.
    Rect src = params->src;
    if(src.max.x <= src.min.x || src.max.y <= src.min.y) {
      src = (Rect){{0, 0}, {params->sprite.size.x, params->sprite.size.y}};
    }
    quad.src.min.x = params->sprite.src.min.x + src.min.x;
    quad.src.min.y = params->sprite.src.min.y + src.min.y;
    quad.src.max.x = params->sprite.src.min.x + src.max.x;
    quad.src.max.y = params->sprite.src.min.y + src.max.y;

    // The mask is on the texture of this same sheet, which d_sprite_masked
    // asserts. Its rect in the sheet therefore passes through with no
    // change.
    if(params->mask.sheet.u64 != 0) {
      quad.mask_src = params->mask.src;
    }

    // A tint of 0 reads as white, which keeps the colors of the sprite. A zoom
    // of 0 reads as 1 in the same way (ZII).
    V4 tint = params->tint;
    if(tint.x == 0 && tint.y == 0 && tint.z == 0 && tint.w == 0) {
      tint = (V4){1, 1, 1, 1};
    }
    for(U32 corner = 0; corner < Corner_COUNT; corner += 1) { quad.colors[corner] = tint; }
    d__emit(&quad);
  }
}

internal void d_sprite(D_Sprite sprite, Rect dst, V4 tint) {
  D_SpriteParams params = {0};
  params.sprite = sprite;
  params.dst = dst;
  params.tint = tint;
  d_sprite_ex(&params);
}

internal void d_sprite_masked(D_Sprite sprite, D_Sprite mask, Rect dst, V4 tint) {
  // A quad reads one texture. A mask that is not nil must therefore be in the
  // sheet of the sprite. Without that rule the shader would read the rect of
  // the mask from the wrong sheet. A nil mask is correct, and it draws the
  // sprite with no mask (ZII).
  Assert(mask.sheet.u64 == 0 || mask.sheet.u64 == sprite.sheet.u64);
  D_SpriteParams params = {0};
  params.sprite = sprite;
  params.dst = dst;
  params.tint = tint;
  params.mask = mask;
  d_sprite_ex(&params);
}

////////////////////////////////
//~ fp: Fonts / Text

internal D_FontSlot* d__font_from_handle(D_Font font) {
  D_FontSlot* result = 0;
  if(font.u64 != 0 && font.u64 <= D_FONT_CAP && d_state.fonts[font.u64 - 1].used) {
    result = &d_state.fonts[font.u64 - 1];
  }
  return result;
}

internal D_Font d_font_open(String8 path) {
  D_Font result = {0};
  if(d_state.initialized) {
    U64 slot = D_FONT_CAP;
    for(U64 i = 0; i < D_FONT_CAP; i += 1) {
      if(!d_state.fonts[i].used) {
        slot = i;
        break;
      }
    }
    if(slot < D_FONT_CAP) {
      B32 read_ok = 0;
      String8 ttf = os_read_entire_file(d_state.arena, path, &read_ok);
      if(read_ok && ttf.size > 0) {
        D_FontSlot* font = &d_state.fonts[slot];
        // GetFontOffsetForIndex reads a plain .ttf file and a .ttc collection.
        int offset = stbtt_GetFontOffsetForIndex(ttf.str, 0);
        if(offset >= 0 && stbtt_InitFont(&font->info, ttf.str, offset)) {
          font->used = 1;
          font->ttf = ttf;
          MemoryZeroArray(font->glyphs);
          result.u64 = slot + 1;
        }
      }
    }
  }
  return result;
}

internal void d_font_close(D_Font font) {
  D_FontSlot* slot = d__font_from_handle(font);
  if(slot != 0) {
    // The bytes of the font and the glyph nodes stay on the arena of the whole
    // program. A font closes seldom, so the memory that a close gives back
    // does not justify an allocator.
    MemoryZeroStruct(slot);
  }
}

internal D_FontMetrics d_font_metrics(D_Font font, F32 size) {
  D_FontMetrics result = {0};
  D_FontSlot* slot = d__font_from_handle(font);
  if(slot != 0) {
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&slot->info, &ascent, &descent, &line_gap);
    F32 scale = stbtt_ScaleForPixelHeight(&slot->info, size); // the units are points
    result.ascent = (F32)ascent * scale;
    result.descent = (F32)(-descent) * scale; // the descent of stbtt is below 0
    result.line_advance = (F32)(ascent - descent + line_gap) * scale;
  }
  return result;
}

//- fp: the decoder of UTF-8. A byte sequence that is not correct gives U+FFFD
//  and moves one byte.
internal U32 d__utf8_next(String8 s, U64* io_off) {
  U64 off = *io_off;
  U32 result = 0xFFFD;
  U8 b = s.str[off];
  if(b < 0x80) {
    result = b;
    off += 1;
  } else if((b & 0xE0) == 0xC0 && off + 1 < s.size && (s.str[off + 1] & 0xC0) == 0x80) {
    result = ((U32)(b & 0x1F) << 6) | (U32)(s.str[off + 1] & 0x3F);
    off += 2;
  } else if((b & 0xF0) == 0xE0 && off + 2 < s.size &&
            (s.str[off + 1] & 0xC0) == 0x80 && (s.str[off + 2] & 0xC0) == 0x80) {
    result = ((U32)(b & 0x0F) << 12) | ((U32)(s.str[off + 1] & 0x3F) << 6) | (U32)(s.str[off + 2] & 0x3F);
    off += 3;
  } else if((b & 0xF8) == 0xF0 && off + 3 < s.size &&
            (s.str[off + 1] & 0xC0) == 0x80 && (s.str[off + 2] & 0xC0) == 0x80 &&
            (s.str[off + 3] & 0xC0) == 0x80) {
    result = ((U32)(b & 0x07) << 18) | ((U32)(s.str[off + 1] & 0x3F) << 12) |
             ((U32)(s.str[off + 2] & 0x3F) << 6) | (U32)(s.str[off + 3] & 0x3F);
    off += 4;
  } else {
    off += 1;
  }
  *io_off = off;
  return result;
}

// Read the cache, and make the raster where the cache has no entry. A glyph
// goes into the glyph sheet, which never closes, at the size in pixels of the
// first call that draws it.
internal D_Glyph* d__glyph(D_FontSlot* font, U32 codepoint, I32 size_px) {
  U64 bucket = ((U64)codepoint * 31 + (U64)size_px) % D_GLYPH_BUCKETS;
  D_Glyph* glyph = 0;
  for(D_Glyph* g = font->glyphs[bucket]; g != 0; g = g->next) {
    if(g->codepoint == codepoint && g->size_px == size_px) {
      glyph = g;
      break;
    }
  }
  if(glyph == 0) {
    glyph = push_array(d_state.arena, D_Glyph, 1);
    glyph->codepoint = codepoint;
    glyph->size_px = size_px;

    F32 scale = stbtt_ScaleForPixelHeight(&font->info, (F32)size_px);
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&font->info, (int)codepoint, &advance, &lsb);
    glyph->advance_px = (F32)advance * scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&font->info, (int)codepoint, scale, scale, &x0, &y0, &x1, &y1);
    I32 w = x1 - x0;
    I32 h = y1 - y0;
    if(w > 0 && h > 0) {
      if(d_state.glyph_sheet == 0) {
        d_state.glyph_sheet = d__sheet_create(D_SHEET_DEFAULT_DIM, D_SHEET_DEFAULT_DIM, R_TexFormat_R8, D_Sampling_Smooth, 0);
      }
      D_Sheet* sheet = d__sheet_from_id(d_state.glyph_sheet);
      I32 px = 0, py = 0;
      if(sheet != 0 && d__sheet_pack(sheet, w, h, &px, &py)) {
        ArenaTemp scratch = arena_get_scratch(0, 0);
        U8* bitmap = push_array_no_zero(scratch.arena, U8, (U64)w * (U64)h);
        stbtt_MakeCodepointBitmap(&font->info, bitmap, w, h, w, scale, scale, (int)codepoint);
        r_tex_update(sheet->tex, px, py, w, h, bitmap);
        arena_release_scratch(scratch);
        glyph->has_bitmap = 1;
        glyph->src = (Rect){{(F32)px, (F32)py}, {(F32)(px + w), (F32)(py + h)}};
        glyph->w_px = (F32)w;
        glyph->h_px = (F32)h;
        glyph->xoff_px = (F32)x0;
        glyph->yoff_px = (F32)y0;
      }
      // The glyph sheet has no space. The cache then holds a glyph with no
      // bitmap, which moves the pen and draws nothing.
    }
    glyph->next = font->glyphs[bucket];
    font->glyphs[bucket] = glyph;
  }
  return glyph;
}

// The walk that the two callers share. It moves the pen across the text, and
// it can draw the quad of each glyph. d_text and d_text_dim call it.
internal F32 d__text_walk(D_Font font, F32 size, V2 pos, V4 color, String8 text, B32 emit) {
  F32 result = pos.x;
  D_FontSlot* slot = d__font_from_handle(font);
  if(slot != 0 && size > 0) {
    F32 to_px = (d_state.scale == 0) ? 1.0f : d_state.scale; // A call to d_text_dim can occur before the first frame.
    I32 size_px = (I32)(size * to_px + 0.5f);
    F32 scale_px = stbtt_ScaleForPixelHeight(&slot->info, (F32)size_px);

    // The walk uses units of points multiplied by the scale. Those units are
    // true pixels for text in screen space. Inside a camera they are units of
    // the world, and the step to the grid of pixels below is then approximate.
    // pos is the top left corner of the box, and each glyph hangs from the
    // baseline.
    F32 baseline = (pos.y + d_font_metrics(font, size).ascent) * to_px;
    F32 pen = pos.x * to_px;
    U32 prev_cp = 0;
    for(U64 off = 0; off < text.size;) {
      U32 cp = d__utf8_next(text, &off);
      if(prev_cp != 0) {
        pen += (F32)stbtt_GetCodepointKernAdvance(&slot->info, (int)prev_cp, (int)cp) * scale_px;
      }
      D_Glyph* glyph = d__glyph(slot, cp, size_px);
      if(emit && glyph->has_bitmap && d_state.frame_arena != 0) {
        // Move the glyph to the grid of pixels. Each texel of the glyph then
        // covers one pixel, which keeps the text sharp under linear
        // sampling.
        F32 gx = roundf(pen + glyph->xoff_px);
        F32 gy = roundf(baseline + glyph->yoff_px);
        R_Quad quad = {0};
        quad.dst = (Rect){{gx / to_px, gy / to_px},
                          {(gx + glyph->w_px) / to_px, (gy + glyph->h_px) / to_px}};
        quad.src = glyph->src;
        quad.tex = d__sheet_from_id(d_state.glyph_sheet)->tex;
        for(U32 corner = 0; corner < Corner_COUNT; corner += 1) { quad.colors[corner] = color; }
        d__emit(&quad);
      }
      pen += glyph->advance_px;
      prev_cp = cp;
    }
    result = pen / to_px;
  }
  return result;
}

internal V2 d_text_dim(D_Font font, F32 size, String8 text) {
  V2 result = {0};
  D_FontSlot* slot = d__font_from_handle(font);
  if(slot != 0) {
    V2 origin = {0};
    V4 no_color = {0};
    result.x = d__text_walk(font, size, origin, no_color, text, 0);
    D_FontMetrics metrics = d_font_metrics(font, size);
    result.y = metrics.ascent + metrics.descent;
  }
  return result;
}

internal F32 d_text(D_Font font, F32 size, V2 pos, V4 color, String8 text) {
  return d__text_walk(font, size, pos, color, text, 1);
}
