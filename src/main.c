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
#include "gfx/render.h"
#include "gfx/window.h"

//- fp: [c]
#include "base/base.c"
#include "tabula.c"
#include "gfx/render.c"
#include "gfx/window.c"

////////////////////////////////
//~ fp: Entry Point

typedef struct {
  B8 key_current[WND_Key_COUNT];
  B8 key_prev[WND_Key_COUNT];
  B8 key_pressed[WND_Key_COUNT]; // latch: went down at any point this frame
  B8 mouse_current[WND_MouseButton_COUNT];
  B8 mouse_prev[WND_MouseButton_COUNT];
  B8 mouse_pressed[WND_MouseButton_COUNT]; // latch, same idea
  V2 mouse_pos;
} Input;

internal B32 input_is_key_down(Input* input, WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input->key_current[key];
}

internal B32 input_is_key_pressed(Input* input, WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input->key_pressed[key];
}

internal B32 input_is_mouse_button_down(Input* input, WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input->mouse_current[mouse];
}

internal B32 input_is_mouse_button_pressed(Input* input, WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input->mouse_pressed[mouse];
}

internal V2 input_mouse_pos(Input* input) {
  return input->mouse_pos;
}

internal void input_process_events(Input* input, WND_EventList event_list) {
  // last frame's state becomes prev, and the per-frame latches reset --
  // BEFORE this frame's events land, or edges can never be observed
  MemoryCopy(input->key_prev, input->key_current, sizeof(input->key_current));
  MemoryCopy(input->mouse_prev, input->mouse_current, sizeof(input->mouse_current));
  MemoryZeroArray(input->key_pressed);
  MemoryZeroArray(input->mouse_pressed);

  for(WND_Event* evt = event_list.first; evt; evt = evt->next) {
    switch(evt->type) {
      case WND_EventType_KeyDown:
        input->key_current[evt->key] = true;
        input->key_pressed[evt->key] = true; // survives a same-frame KeyUp
        break;
      case WND_EventType_KeyUp:
        input->key_current[evt->key] = false;
        break;
      case WND_EventType_MouseDown:
        input->mouse_current[evt->button] = true;
        input->mouse_pressed[evt->button] = true; // survives a same-frame MouseUp
        break;
      case WND_EventType_MouseUp:
        input->mouse_current[evt->button] = false;
        break;
      case WND_EventType_MouseMoved:
        input->mouse_pos = evt->pos;
        break;
      default:
        break;
    }
  }
}

int main(void) {
  TCTX tctx;
  tctx_init_and_equip(&tctx);

  Arena* frame_arena = arena_alloc();
  wnd_open(str8_lit("Imperium"), 1600, 900);
  wnd_equip_gl();
  r_init();


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

    //- fp: temp: render smoke test, until the draw layer exists
    r_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      // flat rounded panel
      R_Quad q = {0};
      q.dst = (Rect){{100, 100}, {500, 300}};
      for(U32 c = 0; c < Corner_COUNT; c += 1) {
        q.colors[c] = (V4){0.2f, 0.4f, 0.9f, 1.0f};
        q.corner_radii[c] = 24.0f;
      }
      q.edge_softness = 1.0f;
      r_push_quad(&q);

      // vertical gradient
      R_Quad g = {0};
      g.dst = (Rect){{550, 100}, {950, 300}};
      g.colors[Corner_TL] = g.colors[Corner_TR] = (V4){0.9f, 0.2f, 0.2f, 1.0f};
      g.colors[Corner_BL] = g.colors[Corner_BR] = (V4){0.9f, 0.8f, 0.1f, 1.0f};
      g.edge_softness = 1.0f;
      r_push_quad(&g);

      // border-only rounded outline
      R_Quad o = {0};
      o.dst = (Rect){{100, 350}, {500, 550}};
      for(U32 c = 0; c < Corner_COUNT; c += 1) {
        o.colors[c] = (V4){1.0f, 1.0f, 1.0f, 1.0f};
        o.corner_radii[c] = 12.0f;
      }
      o.border_thickness = 4.0f;
      o.edge_softness = 1.0f;
      r_push_quad(&o);

      // rotated solid quad
      R_Quad rot = {0};
      rot.dst = (Rect){{600, 380}, {800, 520}};
      for(U32 c = 0; c < Corner_COUNT; c += 1) {
        rot.colors[c] = (V4){0.2f, 0.8f, 0.4f, 1.0f};
        rot.corner_radii[c] = 12.0f;
      }
      rot.rotation = 0.4f;
      rot.edge_softness = 1.0f;
      r_push_quad(&rot);

      // clipped: dst extends to x=1250, clip cuts it at x=1150
      R_Quad clipped = {0};
      clipped.dst = (Rect){{950, 380}, {1250, 520}};
      for(U32 c = 0; c < Corner_COUNT; c += 1) {
        clipped.colors[c] = (V4){0.9f, 0.4f, 0.8f, 1.0f};
      }
      clipped.clip = (Rect){{1000, 400}, {1150, 640}};
      r_push_quad(&clipped);
    }
    r_frame_end();

    wnd_swap(); // vsync: paces the loop to the display rate
    arena_clear(frame_arena);
  }
  wnd_close();

  tctx_release();
  return 0;
}
