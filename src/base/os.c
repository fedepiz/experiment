#include "base/core.h"

////////////////////////////////
//~ fp: Linux Backend

#if OS_LINUX
#include "linux/os.c"
#else
# error The os memory backend is not implemented for this OS yet.
#endif
