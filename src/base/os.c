#include "base/core.h"

////////////////////////////////
//~ fp: The Code For Each Operating System

#if OS_LINUX
#include "base/linux/os.c"
#elif OS_MAC
#include "base/mac/os.c"
#elif OS_WINDOWS
#include "base/win/os.c"
#else
# error The os memory backend is not implemented for this OS yet.
#endif
