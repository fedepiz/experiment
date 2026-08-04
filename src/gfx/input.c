#include "base/core.h"
#include "base/math.h"
#include "gfx/window.h"
#include "gfx/input.h"

////////////////////////////////
//~ fp: Input

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
