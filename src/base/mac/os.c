#include "base/core.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/os.h"

// The OS_MAC guard keeps the file quiet in clangd, which parses it standalone
// on every platform (no POSIX headers off unix); real builds only include it
// on mac anyway, via base/os.c.
#if OS_MAC

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

internal U64 os_page_size(void) {
  return (U64)sysconf(_SC_PAGESIZE);
}

internal void* os_reserve(U64 size) {
  // PROT_NONE: address space only, no pages backing it yet. Darwin has no
  // MAP_NORESERVE and doesn't charge PROT_NONE mappings, so none is needed.
  void* result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(result == MAP_FAILED) { result = 0; }
  return result;
}

internal B32 os_commit(void* ptr, U64 size) {
  B32 result = (mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0);
  return result;
}

internal void os_decommit(void* ptr, U64 size) {
  // MADV_FREE, not MADV_DONTNEED: Darwin treats DONTNEED as a near-no-op;
  // FREE actually lets the kernel reclaim the pages
  madvise(ptr, size, MADV_FREE);
  mprotect(ptr, size, PROT_NONE);
}

internal void os_release(void* ptr, U64 size) {
  munmap(ptr, size);
}

////////////////////////////////
//~ fp: Time

internal U64 os_now_us(void) {
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (U64)ts.tv_sec * 1000000 + (U64)ts.tv_nsec / 1000;
}

////////////////////////////////
//~ fp: Console Output

internal void os_console_write(String8 s, B32 to_stderr) {
  int fd = to_stderr ? 2 : 1;
  // write(2) may write fewer bytes than asked (pipe buffers) -- loop on the
  // remainder; any error (EINTR included) abandons the tail, it's console text
  U64 written = 0;
  while(written < s.size) {
    ssize_t ret = write(fd, s.str + written, s.size - written);
    if(ret <= 0) { break; }
    written += (U64)ret;
  }
}

////////////////////////////////
//~ fp: Files

// paths are String8; syscalls want null-terminated -- a scratch copy bridges
// (push_str8_copy writes the null byte past .size)
internal String8 os_read_entire_file(Arena* arena, String8 path, B32* opt_success) {
  String8 result = {0};
  B32 success = 0;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  String8 path_z = push_str8_copy(scratch.arena, path);
  int fd = open((char*)path_z.str, O_RDONLY);
  arena_release_scratch(scratch);

  if(fd >= 0) {
    struct stat st = {0};
    if(fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
      U64 size = (U64)st.st_size;
      U64 restore_pos = arena_pos(arena);
      U8* buf = push_array_no_zero(arena, U8, size + 1);
      U64 total = 0;
      while(total < size) {
        ssize_t ret = read(fd, buf + total, size - total);
        if(ret <= 0) { break; }
        total += (U64)ret;
      }
      if(total == size) {
        buf[size] = 0;
        result = str8(buf, size);
        success = 1;
      } else {
        arena_pop_to(arena, restore_pos); // don't leak a half-read buffer
      }
    }
    close(fd);
  }
  if(opt_success != 0) { *opt_success = success; }
  return result;
}

internal B32 os_write_entire_file(String8 path, String8 data) {
  B32 result = 0;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  String8 path_z = push_str8_copy(scratch.arena, path);
  int fd = open((char*)path_z.str, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  arena_release_scratch(scratch);

  if(fd >= 0) {
    U64 written = 0;
    while(written < data.size) {
      ssize_t ret = write(fd, data.str + written, data.size - written);
      if(ret <= 0) { break; }
      written += (U64)ret;
    }
    result = (written == data.size);
    close(fd);
  }
  return result;
}

#endif // OS_MAC
