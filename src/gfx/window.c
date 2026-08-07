#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
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
