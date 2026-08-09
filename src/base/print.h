#pragma once

#include "base/core.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Printing
//
// A format goes onto a scratch arena. The whole string then goes out in one
// write to the operating system, with no buffer. These functions use no part
// of stdio, and they add no new line. %S writes a String8, as in
// printf_str8("hi %S\n", name).

internal void print_str8(String8 s);
internal void printf_str8(char* fmt, ...);

//- fp: the same functions, which write to stderr
internal void eprint_str8(String8 s);
internal void eprintf_str8(char* fmt, ...);
