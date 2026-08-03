#include "base/core.h"
#include "base/os.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/print.h"

////////////////////////////////
//~ fp: Printing

internal void print_str8(String8 s) {
  os_console_write(s, 0);
}

internal void eprint_str8(String8 s) {
  os_console_write(s, 1);
}

internal void printf_str8(char* fmt, ...) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 s = push_str8fv(scratch.arena, fmt, args);
  va_end(args);
  print_str8(s);
  arena_release_scratch(scratch);
}

internal void eprintf_str8(char* fmt, ...) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 s = push_str8fv(scratch.arena, fmt, args);
  va_end(args);
  eprint_str8(s);
  arena_release_scratch(scratch);
}
