#include "base/core.h"

////////////////////////////////
//~ fp: X11 Backend
//
// Xlib pollutes the TU with typedefs and macros (Window, Display, True, None,
// ...) -- in a unity build that pollution is global, so avoid those names for
// our own symbols.

#if OS_LINUX
#include "linux/window.c"
#else
# error The window layer is not implemented for this OS yet.
#endif
