#include "base/core.h"
#include "base/math.h"
#include "gfx/window.h"
#include "gfx/input.h"

////////////////////////////////
//~ fp: Input

typedef struct {
  B8 key_current[WND_Key_COUNT];
  B8 key_prev[WND_Key_COUNT];
  B8 key_pressed[WND_Key_COUNT]; // latch: went down at any point this frame
  B8 mouse_current[WND_MouseButton_COUNT];
  B8 mouse_prev[WND_MouseButton_COUNT];
  B8 mouse_pressed[WND_MouseButton_COUNT]; // latch, same idea
  V2 mouse_pos;
} Input;

global Input input_state;

internal B32 input_is_key_down(WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input_state.key_current[key];
}

internal B32 input_is_key_pressed(WND_Key key) {
  if(key >= WND_Key_COUNT) return false;
  return input_state.key_pressed[key];
}

internal B32 input_is_mouse_button_down(WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input_state.mouse_current[mouse];
}

internal B32 input_is_mouse_button_pressed(WND_MouseButton mouse) {
  if(mouse >= WND_MouseButton_COUNT) return false;
  return input_state.mouse_pressed[mouse];
}

internal V2 input_mouse_pos(void) {
  return input_state.mouse_pos;
}

internal void input_process_events(WND_EventList event_list) {
  // last frame's state becomes prev, and the per-frame latches reset --
  // BEFORE this frame's events land, or edges can never be observed
  MemoryCopy(input_state.key_prev, input_state.key_current, sizeof(input_state.key_current));
  MemoryCopy(input_state.mouse_prev, input_state.mouse_current, sizeof(input_state.mouse_current));
  MemoryZeroArray(input_state.key_pressed);
  MemoryZeroArray(input_state.mouse_pressed);

  for(WND_Event* evt = event_list.first; evt; evt = evt->next) {
    switch(evt->type) {
      case WND_EventType_KeyDown:
        input_state.key_current[evt->key] = true;
        input_state.key_pressed[evt->key] = true; // survives a same-frame KeyUp
        break;
      case WND_EventType_KeyUp:
        input_state.key_current[evt->key] = false;
        break;
      case WND_EventType_MouseDown:
        input_state.mouse_current[evt->button] = true;
        input_state.mouse_pressed[evt->button] = true; // survives a same-frame MouseUp
        break;
      case WND_EventType_MouseUp:
        input_state.mouse_current[evt->button] = false;
        break;
      case WND_EventType_MouseMoved:
        input_state.mouse_pos = evt->pos;
        break;
      default:
        break;
    }
  }
}
