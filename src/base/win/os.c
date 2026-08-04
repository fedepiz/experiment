#include "base/core.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/os.h"

////////////////////////////////
//~ fp: Windows Backend
//
// Win32 pollutes the unity TU even harder than Xlib (min/max, near/far,
// thousands of typedefs) -- WIN32_LEAN_AND_MEAN and NOMINMAX keep the worst
// of it out. This is the first windows.h in the TU; the gfx backend includes
// it again behind the same guards, harmlessly.
//
// The OS_WINDOWS guard keeps the file quiet in clangd, which parses it
// standalone on every platform (no windows.h off Windows); real builds only
// include it on Windows anyway, via base/os.c.

#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

////////////////////////////////
//~ fp: Virtual Memory

internal U64 os_page_size(void) {
  SYSTEM_INFO info = {0};
  GetSystemInfo(&info);
  return (U64)info.dwPageSize;
}

internal void* os_reserve(U64 size) {
  // MEM_RESERVE: address space only, no pages backing it yet -- VirtualAlloc
  // is the API the reserve/commit split in os.h is modeled on
  void* result = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
  return result;
}

internal B32 os_commit(void* ptr, U64 size) {
  B32 result = (VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != 0);
  return result;
}

internal void os_decommit(void* ptr, U64 size) {
  VirtualFree(ptr, size, MEM_DECOMMIT);
}

internal void os_release(void* ptr, U64 size) {
  // MEM_RELEASE frees the whole reservation from its base and demands size 0
  Unused(size);
  VirtualFree(ptr, 0, MEM_RELEASE);
}

////////////////////////////////
//~ fp: Time

internal U64 os_now_us(void) {
  // QPC frequency is fixed at boot; fetch once
  local_persist U64 freq = 0;
  if(freq == 0) {
    LARGE_INTEGER f = {0};
    QueryPerformanceFrequency(&f);
    freq = (U64)f.QuadPart;
  }
  LARGE_INTEGER counter = {0};
  QueryPerformanceCounter(&counter);
  U64 ticks = (U64)counter.QuadPart;
  // whole seconds and the remainder scale separately: ticks * 1000000 in one
  // go overflows 64 bits within hours at a 10MHz QPC
  return (ticks / freq) * 1000000 + (ticks % freq) * 1000000 / freq;
}

////////////////////////////////
//~ fp: Console Output

internal void os_console_write(String8 s, B32 to_stderr) {
  HANDLE handle = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  if(handle != INVALID_HANDLE_VALUE && handle != 0) {
    // WriteFile counts in DWORDs; chunk-and-loop covers >4GB strings and the
    // usual partial-write cases in one shape
    U64 written = 0;
    while(written < s.size) {
      DWORD chunk = (DWORD)ClampTop(s.size - written, MB(256));
      DWORD ret = 0;
      if(!WriteFile(handle, s.str + written, chunk, &ret, 0) || ret == 0) { break; }
      written += ret;
    }
  }
}

////////////////////////////////
//~ fp: Files

// paths are UTF-8 String8; only the wide-char APIs speak Unicode reliably (the
// *A functions run through the legacy ANSI codepage and mangle anything
// outside it), so a scratch UTF-16 copy bridges instead of the usual
// push_str8_copy
internal WCHAR* os_win__wide_from_str8(Arena* arena, String8 s) {
  int count = 0;
  if(s.size > 0) {
    count = MultiByteToWideChar(CP_UTF8, 0, (char*)s.str, (int)s.size, 0, 0);
  }
  WCHAR* result = push_array_no_zero(arena, WCHAR, (U64)count + 1);
  if(count > 0) {
    MultiByteToWideChar(CP_UTF8, 0, (char*)s.str, (int)s.size, result, count);
  }
  result[count] = 0;
  return result;
}

internal String8 os_read_entire_file(Arena* arena, String8 path, B32* opt_success) {
  String8 result = {0};
  B32 success = 0;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  WCHAR* path_w = os_win__wide_from_str8(scratch.arena, path);
  // directories fail this open (they need FILE_FLAG_BACKUP_SEMANTICS), which
  // stands in for the S_ISREG check the POSIX backends make
  HANDLE file = CreateFileW(path_w, GENERIC_READ, FILE_SHARE_READ, 0,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  arena_release_scratch(scratch);

  if(file != INVALID_HANDLE_VALUE) {
    LARGE_INTEGER file_size = {0};
    if(GetFileSizeEx(file, &file_size)) {
      U64 size = (U64)file_size.QuadPart;
      U64 restore_pos = arena_pos(arena);
      U8* buf = push_array_no_zero(arena, U8, size + 1);
      U64 total = 0;
      while(total < size) {
        DWORD chunk = (DWORD)ClampTop(size - total, MB(256));
        DWORD ret = 0;
        if(!ReadFile(file, buf + total, chunk, &ret, 0) || ret == 0) { break; }
        total += ret;
      }
      if(total == size) {
        buf[size] = 0;
        result = str8(buf, size);
        success = 1;
      } else {
        arena_pop_to(arena, restore_pos); // don't leak a half-read buffer
      }
    }
    CloseHandle(file);
  }
  if(opt_success != 0) { *opt_success = success; }
  return result;
}

internal B32 os_write_entire_file(String8 path, String8 data) {
  B32 result = 0;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  WCHAR* path_w = os_win__wide_from_str8(scratch.arena, path);
  HANDLE file = CreateFileW(path_w, GENERIC_WRITE, 0, 0,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  arena_release_scratch(scratch);

  if(file != INVALID_HANDLE_VALUE) {
    U64 written = 0;
    while(written < data.size) {
      DWORD chunk = (DWORD)ClampTop(data.size - written, MB(256));
      DWORD ret = 0;
      if(!WriteFile(file, data.str + written, chunk, &ret, 0) || ret == 0) { break; }
      written += ret;
    }
    result = (written == data.size);
    CloseHandle(file);
  }
  return result;
}

#endif // OS_WINDOWS
