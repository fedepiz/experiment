#pragma once

#include "base/core.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Virtual Memory
//
// A reserve and a commit are two different operations. A reserve takes
// addresses only, and a reserve of many gigabytes costs nothing. A commit puts
// pages of memory behind a part of those addresses. An arena uses these two
// operations to grow, and it never moves its memory.

internal U64   os_page_size(void);
internal void* os_reserve(U64 size);
internal B32   os_commit(void* ptr, U64 size);
internal void  os_decommit(void* ptr, U64 size);
internal void  os_release(void* ptr, U64 size);

////////////////////////////////
//~ fp: Time

// Microseconds that always increase, from a start point with no meaning. The
// difference between two values has a meaning. One value alone does not.
internal U64 os_now_us(void);

////////////////////////////////
//~ fp: Console Output
//
// These functions use no buffer. The print layer makes the whole string first,
// then gives it to one call.

internal void os_console_write(String8 s, B32 to_stderr);

////////////////////////////////
//~ fp: Files
//
// These functions read and write a whole file, and that is deliberate. A file
// becomes a String8 on your arena, and the string functions then take parts of
// it, as with any other string. A function that reads a part of a file can go
// below these functions, if a file becomes too large to read at one time.
//
// A read always gives a result. A file that is absent, and a file that the
// program cannot read, both give an empty String8, as an empty file does. A
// caller that must know the difference gives `opt_success` and examines it. A
// caller that does not give 0 and uses the empty result (ZII).

internal String8 os_read_entire_file(Arena* arena, String8 path, B32* opt_success);
internal B32     os_write_entire_file(String8 path, String8 data);
