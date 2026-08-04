#include "base/core.h"
#include "base/math.h"
#include "base/os.h"
#include "gfx/window.h"

////////////////////////////////
//~ fp: Frame Timing
//
// Backend-independent: backends call wnd__frame_mark from wnd_swap, this
// keeps the delta.

global U64 wnd__frame_last_us;
global F32 wnd__frame_dt;

internal void wnd__frame_mark(void) {
  U64 now = os_now_us();
  if(wnd__frame_last_us != 0) {
    wnd__frame_dt = (F32)(now - wnd__frame_last_us) / 1000000.0f;
  }
  wnd__frame_last_us = now;
}

internal F32 wnd_frame_time(void) {
  return wnd__frame_dt;
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
