#pragma once

#include "base/core.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Printing
//
// Formatting happens on a scratch arena, then the whole string goes out in a
// single unbuffered os write. No libc stdio anywhere. No automatic newline.
// %S formats a String8: printf_str8("hi %S\n", name)

internal void print_str8(String8 s);
internal void printf_str8(char* fmt, ...);

//- fp: stderr twins
internal void eprint_str8(String8 s);
internal void eprintf_str8(char* fmt, ...);
