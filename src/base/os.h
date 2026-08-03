#pragma once

#include "base/core.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Virtual Memory
//
// The reserve/commit split is the whole point: reserving grabs address space
// only (a multi-GB reservation costs nothing), committing backs a subrange
// with actual pages. The arena leans on this to grow without ever moving.

internal U64   os_page_size(void);
internal void* os_reserve(U64 size);
internal B32   os_commit(void* ptr, U64 size);
internal void  os_decommit(void* ptr, U64 size);
internal void  os_release(void* ptr, U64 size);

////////////////////////////////
//~ fp: Console Output
//
// Unbuffered; the print layer formats a whole string first and hands it over
// in one call.

internal void os_console_write(String8 s, B32 to_stderr);

////////////////////////////////
//~ fp: Files
//
// Whole-file operations only, by design: a file becomes a String8 on your
// arena, sliced with the string tools like anything else. Streaming handles
// can slot in beneath these if a too-big-to-slurp case ever appears.
//
// Reads always "just work": a missing/unreadable file reads the same as an
// empty one -- an empty String8. Callers that care pass opt_success and check
// it; callers that don't pass 0 and lean on ZII.

internal String8 os_read_entire_file(Arena* arena, String8 path, B32* opt_success);
internal B32     os_write_entire_file(String8 path, String8 data);
