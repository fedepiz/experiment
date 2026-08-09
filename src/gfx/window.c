#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/os.h"
#include "gfx/window.h"

////////////////////////////////
//~ fp: Frame Timing
//
// This code does not depend on a backend. Each backend calls wnd__frame_mark
// in wnd_swap, and this code holds the time between two such calls.

global U64 wnd__frame_last_us;
global F32 wnd__frame_dt;
global F32 wnd__frame_dt_raw;
global F32 wnd__frame_debt; // the seconds of measured time that no frame reported yet

internal void wnd__frame_mark(void) {
  U64 now = os_now_us();
  if(wnd__frame_last_us != 0) {
    F32 dt = (F32)(now - wnd__frame_last_us) / 1000000.0f;
    wnd__frame_dt_raw = dt;
    // Give the time as whole periods of the display, and hold the difference
    // from the measurement as a debt. See the header. In the usual state this
    // code gives exactly one period for each frame, and the movement of the
    // raw measurement changes nothing: a block of 17ms and then a return after
    // 0.5ms, which the queue absorbed, read as two equal periods, which is
    // what the display showed. A true slowdown moves the debt past the limit,
    // and the code then gives more whole periods at once. A stall therefore
    // stays a stall.
    F32 hz = wnd_refresh_rate();
    if(hz > 0) {
      F32 period = 1.0f / hz;
      wnd__frame_debt += dt - period;
      dt = period;
      while(wnd__frame_debt > 1.5f * period) { dt += period; wnd__frame_debt -= period; }
      while(wnd__frame_debt < -0.5f * period && dt > 0) { dt -= period; wnd__frame_debt += period; }
    }
    wnd__frame_dt = dt;
  }
  wnd__frame_last_us = now;
}

internal F32 wnd_frame_time(void) {
  return wnd__frame_dt;
}

internal F32 wnd_frame_time_raw(void) {
  return wnd__frame_dt_raw;
}

////////////////////////////////
//~ fp: Events

internal WND_Event* wnd__push_event(Arena* arena, WND_EventList* list, WND_EventType type) {
  WND_Event* event = push_array(arena, WND_Event, 1);
  event->type = type;
  SLLQueuePush(list->first, list->last, event);
  list->count += 1;
  return event;
}

////////////////////////////////
//~ fp: Input
//
// The digest that the code above reads. An event exists for the length of one
// poll: the backend puts it on a scratch arena, this code reads it into the
// digest, and the arena then gives the memory back.

typedef struct {
  B8 key_down[WND_Key_COUNT];
  B8 key_pressed[WND_Key_COUNT]; // It holds for the frame: the key went down at some point of this frame.
  B8 mouse_down[WND_MouseButton_COUNT];
  B8 mouse_pressed[WND_MouseButton_COUNT];  // It holds for the frame, as key_pressed does.
  B8 mouse_released[WND_MouseButton_COUNT]; // It holds for the frame: the button went up at some point of this frame.
  B8 close_requested;
  V2 mouse_pos;
  V2 scroll; // the sum across the frame, because the wheel can make more than one event
} WND_InputState;

global WND_InputState wnd__input;

internal B32 wnd_close_requested(void) {
  return wnd__input.close_requested;
}

internal B32 wnd_key_down(WND_Key key) {
  Assert(key < WND_Key_COUNT);
  return wnd__input.key_down[key];
}

internal B32 wnd_key_pressed(WND_Key key) {
  Assert(key < WND_Key_COUNT);
  return wnd__input.key_pressed[key];
}

internal B32 wnd_mouse_down(WND_MouseButton button) {
  Assert(button < WND_MouseButton_COUNT);
  return wnd__input.mouse_down[button];
}

internal B32 wnd_mouse_pressed(WND_MouseButton button) {
  Assert(button < WND_MouseButton_COUNT);
  return wnd__input.mouse_pressed[button];
}

internal B32 wnd_mouse_released(WND_MouseButton button) {
  Assert(button < WND_MouseButton_COUNT);
  return wnd__input.mouse_released[button];
}

internal V2 wnd_mouse_pos(void) {
  return wnd__input.mouse_pos;
}

internal V2 wnd_scroll(void) {
  return wnd__input.scroll;
}

internal void wnd_poll(void) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  WND_EventList events = wnd__get_events(scratch.arena);

  // Clear the members that hold for one frame BEFORE the events of this frame
  // arrive. Without that order a caller could never read an edge.
  MemoryZeroArray(wnd__input.key_pressed);
  MemoryZeroArray(wnd__input.mouse_pressed);
  MemoryZeroArray(wnd__input.mouse_released);
  MemoryZeroStruct(&wnd__input.scroll);
  wnd__input.close_requested = 0;

  for(WND_Event* evt = events.first; evt; evt = evt->next) {
    switch(evt->type) {
      case WND_EventType_KeyDown:
        wnd__input.key_down[evt->key] = true;
        wnd__input.key_pressed[evt->key] = true; // a KeyUp in this same frame does not clear it
        break;
      case WND_EventType_KeyUp:
        wnd__input.key_down[evt->key] = false;
        break;
      case WND_EventType_MouseDown:
        wnd__input.mouse_down[evt->button] = true;
        wnd__input.mouse_pressed[evt->button] = true; // a MouseUp in this same frame does not clear it
        break;
      case WND_EventType_MouseUp:
        wnd__input.mouse_down[evt->button] = false;
        wnd__input.mouse_released[evt->button] = true; // a MouseDown in this same frame does not clear it
        break;
      case WND_EventType_MouseMoved:
        wnd__input.mouse_pos = evt->pos;
        break;
      case WND_EventType_Scroll:
        wnd__input.scroll = v2_add(wnd__input.scroll, evt->scroll);
        break;
      case WND_EventType_CloseRequested:
        wnd__input.close_requested = 1;
        break;
      default:
        break;
    }
  }

  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Per-OS Backend
//
// Xlib adds many typedefs and macros to this translation unit: Window,
// Display, True, None and more. This is a unity build, so those names become
// global. Do not use one of them for a symbol of this program. The Mac backend
// has no such problem, because Cocoa stays behind the C layer in mac/cocoa.h
// and mac/cocoa.m, which is a separate translation unit.

#if OS_LINUX
#include "gfx/linux/window.c"
#elif OS_MAC
#include "gfx/mac/window.c"
#elif OS_WINDOWS
#include "gfx/win/window.c"
#else
# error The window layer is not implemented for this OS yet.
#endif
