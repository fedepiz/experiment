#include "base/core.h"

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
