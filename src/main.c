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
#include "window.h"

//- fp: [c]
#include "base/base.c"
#include "tabula.c"
#include "window.c"

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

B32 input_is_key_down(Input* input, WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input->key_current[key];
}

B32 input_is_key_pressed(Input* input, WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input->key_pressed[key];
}

B32 input_is_mouse_button_down(Input* input, WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input->mouse_current[mouse];
}

B32 input_is_mouse_button_pressed(Input* input, WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input->mouse_pressed[mouse];
}

V2 input_mouse_pos(Input* input) {
  return input->mouse_pos;
}

void input_process_events(Input* input, WND_EventList event_list) {
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


  Input input = {0};

  for(B32 keep_going = true; keep_going;) {
    WND_EventList evts = wnd_get_events(frame_arena);
    for(WND_Event* evt = evts.first; evt; evt = evt->next) {
      if(evt->type == WND_EventType_CloseRequested) {
        keep_going = false;
      }
    }

    input_process_events(&input, evts);

    if(input_is_key_down(&input, WND_Key_Space)) {
      keep_going = false;
    }

    if(input_is_mouse_button_pressed(&input, WND_MouseButton_Left)) {
      V2 pos = input_mouse_pos(&input);
      printf_str8("(%f, %f)\n", pos.x, pos.y);
    }

    arena_clear(frame_arena);
  }
  wnd_close();

  tctx_release();
  return 0;
}
