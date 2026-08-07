#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/os.h"
#include "gfx/window.h"

////////////////////////////////
//~ fp: Frame Timing
//
// Backend-independent: backends call wnd__frame_mark from wnd_swap, this
// keeps the delta.

global U64 wnd__frame_last_us;
global F32 wnd__frame_dt;
global F32 wnd__frame_dt_raw;
global F32 wnd__frame_debt; // measured-but-unreported wall time, seconds

internal void wnd__frame_mark(void) {
  U64 now = os_now_us();
  if(wnd__frame_last_us != 0) {
    F32 dt = (F32)(now - wnd__frame_last_us) / 1000000.0f;
    wnd__frame_dt_raw = dt;
    // pay time out in whole display periods, banking the difference from the
    // measurement as a debt (see the header). The steady state reports
    // exactly one period per frame no matter how the raw unblock times
    // wobble -- a 17ms block followed by a 0.5ms queue-absorbed return reads
    // as two even periods, which is what the display actually showed. Real
    // slowdowns push the debt past the threshold and pay out extra whole
    // periods immediately, so a stall is still a stall.
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
// The digest the frontier reads. Events live only for the length of a poll:
// they are pumped onto scratch, folded in here, and dropped.

typedef struct {
  B8 key_down[WND_Key_COUNT];
  B8 key_pressed[WND_Key_COUNT]; // latch: went down at any point this frame
  B8 mouse_down[WND_MouseButton_COUNT];
  B8 mouse_pressed[WND_MouseButton_COUNT];  // latch, same idea
  B8 mouse_released[WND_MouseButton_COUNT]; // latch: went up at any point this frame
  B8 close_requested;
  V2 mouse_pos;
  V2 scroll; // accumulated over the frame: several wheel events sum
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

  // the per-frame latches reset BEFORE this frame's events land, or edges
  // can never be observed
  MemoryZeroArray(wnd__input.key_pressed);
  MemoryZeroArray(wnd__input.mouse_pressed);
  MemoryZeroArray(wnd__input.mouse_released);
  MemoryZeroStruct(&wnd__input.scroll);
  wnd__input.close_requested = 0;

  for(WND_Event* evt = events.first; evt; evt = evt->next) {
    switch(evt->type) {
      case WND_EventType_KeyDown:
        wnd__input.key_down[evt->key] = true;
        wnd__input.key_pressed[evt->key] = true; // survives a same-frame KeyUp
        break;
      case WND_EventType_KeyUp:
        wnd__input.key_down[evt->key] = false;
        break;
      case WND_EventType_MouseDown:
        wnd__input.mouse_down[evt->button] = true;
        wnd__input.mouse_pressed[evt->button] = true; // survives a same-frame MouseUp
        break;
      case WND_EventType_MouseUp:
        wnd__input.mouse_down[evt->button] = false;
        wnd__input.mouse_released[evt->button] = true; // survives a same-frame MouseDown
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
// Xlib pollutes the TU with typedefs and macros (Window, Display, True, None,
// ...) -- in a unity build that pollution is global, so avoid those names for
// our own symbols. The mac backend dodges the equivalent problem: Cocoa stays
// behind the C compatibility layer in mac/cocoa.[hm], its own TU.

#if OS_LINUX
#include "gfx/linux/window.c"
#elif OS_MAC
#include "gfx/mac/window.c"
#elif OS_WINDOWS
#include "gfx/win/window.c"
#else
# error The window layer is not implemented for this OS yet.
#endif
