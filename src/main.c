////////////////////////////////
//~ fp: Layer Includes
//
// The only file the compiler is pointed at. All headers first, then all
// implementations -- that ordering is why the .h/.c split still earns its keep
// in a unity build.

//- fp: [h]
#include "base/arena.h"
#include "base/base.h"
#include "base/core.h"
#include "base/print.h"
#include "base/strings.h"
#include "tabula.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/input.h"
#include "gfx/draw.h"


////////////////////////////////
//~ fp: Entry Point

int main(void) {
  TCTX tctx;
  tctx_init_and_equip(&tctx);

  Arena* frame_arena = arena_alloc();
  wnd_open(str8_lit("Imperium"), 1600, 900);
  wnd_equip_gl();
  d_init();

  //- fp: temp: demo assets for the draw smoke test
  D_Font font = d_font_open(str8_lit("/System/Library/Fonts/Supplemental/Arial.ttf"));
  D_Sprite checker = {0};
  D_Sprite stripes = {0};
  {
    enum { SPRITE_DIM = 128 };
    D_Image checker_img = d_image_create(frame_arena, SPRITE_DIM, SPRITE_DIM, (V4){0});
    D_Image stripes_img = d_image_create(frame_arena, SPRITE_DIM, SPRITE_DIM, (V4){0});
    for(I32 y = 0; y < SPRITE_DIM; y += 1) {
      for(I32 x = 0; x < SPRITE_DIM; x += 1) {
        B32 c_on = (((x >> 4) + (y >> 4)) & 1);
        d_image_set_px(checker_img, x, y, c_on ? (V4){0.90f, 0.35f, 0.78f, 1} : (V4){0.16f, 0.16f, 0.19f, 1});
        B32 s_on = (((x + y) >> 4) & 1);
        d_image_set_px(stripes_img, x, y, s_on ? (V4){0.24f, 0.82f, 0.75f, 1} : (V4){0.08f, 0.24f, 0.24f, 1});
      }
    }
    d_spritesheet_begin(512, 256);
    checker = d_spritesheet_push(checker_img);
    stripes = d_spritesheet_push(stripes_img);
    d_spritesheet_end();
    arena_clear(frame_arena); // pixels are on the GPU now
  }


  D_Camera camera = {0};
  camera.zoom = 2;
  F32 stripes_angle = 0;

  for(B32 keep_going = true; keep_going;) {
    WND_EventList evts = wnd_get_events(frame_arena);
    for(WND_Event* evt = evts.first; evt; evt = evt->next) {
      if(evt->type == WND_EventType_CloseRequested) {
        keep_going = false;
      }
    }

    input_process_events(evts);

    if(input_is_key_down(WND_Key_Escape)) {
      keep_going = false;
    }

    if(input_is_mouse_button_pressed(WND_MouseButton_Left)) {
      V2 pos = input_mouse_pos();
      printf_str8("(%f, %f)\n", pos.x, pos.y);
    }

    //- fp: temp: draw smoke test
    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      V2 vp = wnd_size();
      d_rect_gradient_v((Rect){{0, 0}, {vp.x, vp.y}},
                        (V4){0.09f, 0.09f, 0.13f, 1}, (V4){0.03f, 0.03f, 0.05f, 1});

      d_rect_rounded((Rect){{100, 100}, {500, 300}}, (V4){0.2f, 0.4f, 0.9f, 1}, 24);
      d_rect_outline((Rect){{100, 350}, {500, 550}}, (V4){1, 1, 1, 1}, 4);

      d_sprite(checker, (Rect){{550, 100}, {806, 356}}, (V4){0}); // zero tint = as-is
      stripes_angle += 0.5f * wnd_frame_time();                   // half a radian per second
      D_SpriteParams sp = {0};
      sp.sprite = stripes;
      sp.dst = (Rect){{880, 120}, {1080, 320}};
      sp.rotation = stripes_angle;
      d_sprite_ex(&sp);

      d_push_clip((Rect){{1150, 120}, {1350, 320}});
      d_rect((Rect){{1100, 100}, {1500, 340}}, (V4){0.9f, 0.4f, 0.8f, 1});
      d_pop_clip();

      d_line((V2){550, 420}, (V2){1050, 480}, 3, (V4){1.0f, 0.8f, 0.2f, 1});

      d_camera_begin(camera);
      d_rect_rounded((Rect){{-60, 120}, {60, 200}}, (V4){0.2f, 0.8f, 0.4f, 1}, 10);
      d_camera_end();

      d_text(font, 40, (V2){100, 600}, Col_White,
             str8_lit("Imperium — draw layer online"));
      d_text(font, 18, (V2){100, 660}, col_rgb(0.7f, 0.7f, 0.75f),
             str8_lit("rects, sprites, sheets, clip, camera, text"));

      // hue ramp via col_hsva, fading out via col_with_alpha
      for(I32 i = 0; i < 24; i += 1) {
        F32 t = (F32)i / 24.0f;
        d_rect((Rect){{100 + (F32)i * 30, 720}, {128 + (F32)i * 30, 760}},
               col_with_alpha(col_hsva(t, 0.85f, 1.0f, 1.0f), 1.0f - t * 0.7f));
      }

      // Camera movmements
      {
        struct {
          WND_Key key;
          F32 x;
          F32 y;
        } INTERACTIONS[] = {
            {WND_Key_A, -1, 0},
            {WND_Key_D, 1, 0},
            {WND_Key_W, 0, -1},
            {WND_Key_S, 0, 1},
        };

        V2 d_trans = {0};

        for(U32 i = 0; i < ArrayCount(INTERACTIONS); ++i) {
          if(input_is_key_down((INTERACTIONS[i].key))) {
            d_trans.x += INTERACTIONS[i].x;
            d_trans.y += INTERACTIONS[i].y;
          }
        }
        F32 delta = wnd_frame_time() * 100;

        camera.center = v2_scaled_add(camera.center, d_trans, delta);

        // scroll zooms, exponentially so steps feel even at any level
        F32 zoom = (camera.zoom == 0) ? 1.0f : camera.zoom;
        zoom *= 1.0f + 0.1f * input_scroll().y;
        camera.zoom = Clamp(0.25f, zoom, 8.0f);
      }
    }
    d_frame_end();

    wnd_swap(); // vsync: paces the loop to the display rate
    arena_clear(frame_arena);
  }
  wnd_close();

  tctx_release();
  return 0;
}

//- fp: [c]
#include "base/base.c"
#include "tabula.c"
#include "gfx/gfx.c"
