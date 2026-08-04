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
    U8* checker_px = push_array(frame_arena, U8, SPRITE_DIM * SPRITE_DIM * 4);
    U8* stripes_px = push_array(frame_arena, U8, SPRITE_DIM * SPRITE_DIM * 4);
    for(I32 y = 0; y < SPRITE_DIM; y += 1) {
      for(I32 x = 0; x < SPRITE_DIM; x += 1) {
        U8* c = &checker_px[(y * SPRITE_DIM + x) * 4];
        B32 c_on = (((x >> 4) + (y >> 4)) & 1);
        c[0] = c_on ? 230 : 40;
        c[1] = c_on ? 90 : 40;
        c[2] = c_on ? 200 : 48;
        c[3] = 255;
        U8* s = &stripes_px[(y * SPRITE_DIM + x) * 4];
        B32 s_on = (((x + y) >> 4) & 1);
        s[0] = s_on ? 60 : 20;
        s[1] = s_on ? 210 : 60;
        s[2] = s_on ? 190 : 60;
        s[3] = 255;
      }
    }
    d_spritesheet_begin(512, 256);
    checker = d_spritesheet_push((D_Image){SPRITE_DIM, SPRITE_DIM, checker_px});
    stripes = d_spritesheet_push((D_Image){SPRITE_DIM, SPRITE_DIM, stripes_px});
    d_spritesheet_end();
    arena_clear(frame_arena); // pixels are on the GPU now
  }


  Input input = {0};

  for(B32 keep_going = true; keep_going;) {
    WND_EventList evts = wnd_get_events(frame_arena);
    for(WND_Event* evt = evts.first; evt; evt = evt->next) {
      if(evt->type == WND_EventType_CloseRequested) {
        keep_going = false;
      }
    }

    input_process_events(&input, evts);

    if(input_is_key_down(&input, WND_Key_Escape)) {
      keep_going = false;
    }

    if(input_is_mouse_button_pressed(&input, WND_MouseButton_Left)) {
      V2 pos = input_mouse_pos(&input);
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
      D_SpriteParams sp = {0};
      sp.sprite = stripes;
      sp.dst = (Rect){{880, 120}, {1080, 320}};
      sp.rotation = 0.4f;
      d_sprite_ex(&sp);

      d_push_clip((Rect){{1150, 120}, {1350, 320}});
      d_rect((Rect){{1100, 100}, {1500, 340}}, (V4){0.9f, 0.4f, 0.8f, 1});
      d_pop_clip();

      d_line((V2){550, 420}, (V2){1050, 480}, 3, (V4){1.0f, 0.8f, 0.2f, 1});

      d_camera_begin((D_Camera){{0, 0}, 2});
      d_rect_rounded((Rect){{-60, 120}, {60, 200}}, (V4){0.2f, 0.8f, 0.4f, 1}, 10);
      d_camera_end();

      d_text(font, 40, (V2){100, 600}, (V4){1, 1, 1, 1},
             str8_lit("Imperium — draw layer online"));
      d_text(font, 18, (V2){100, 660}, (V4){0.7f, 0.7f, 0.75f, 1},
             str8_lit("rects, sprites, sheets, clip, camera, text"));
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
