#include "base/core.h"

////////////////////////////////
//~ fp: Windows Backend
//
// VirtualAlloc(MEM_RESERVE / MEM_COMMIT) maps directly onto the os.h
// reserve/commit split when the time comes.
//
// The OS_WINDOWS guard keeps the error out of clangd, which parses this file
// standalone on every platform; a real Windows build still trips it.

#if OS_WINDOWS
# error The os backend is not implemented for Windows yet.
#endif
