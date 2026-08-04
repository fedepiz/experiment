#include "base/core.h"

////////////////////////////////
//~ fp: Win32 Backend
//
// The OS_WINDOWS guard keeps the error out of clangd, which parses this file
// standalone on every platform; a real Windows build still trips it.

#if OS_WINDOWS
# error The window layer is not implemented for Windows yet.
#endif
