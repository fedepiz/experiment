#pragma once

#include <stdarg.h>

#include "base/core.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: String8
//
// A String8 is a pointer and a length. It has no null byte at its end, and it
// owns no memory.
//
// A String8 is a view. A function that takes a part of a string, and a function
// that divides a string, both point into the memory that exists. The push_*
// functions are the only functions that allocate.
//
// A string that a push_* function makes does have one null byte after `size`,
// so you can give its .str to a C function directly.

typedef struct {
  U8* str;
  U64 size;
} String8;

//- fp: constructors
internal String8 str8(U8* str, U64 size);
internal String8 str8_cstring(const char* cstr);
#define str8_lit(s)      str8((U8*)(s), sizeof(s) - 1)
#define str8_lit_comp(s) {(U8*)(s), sizeof(s) - 1} // the form with braces, for a static initializer

//- fp: the bridge to printf: printf("%.*s", str8_varg(s))
#define str8_varg(s) (int)(s).size, (s).str

////////////////////////////////
//~ fp: Parts Of A String -- these functions do not allocate

// substr clamps both limits. It includes `first`. It does not include `opl`,
// which is the position one past the last byte.
internal String8 str8_substr(String8 s, U64 first, U64 opl);
internal String8 str8_prefix(String8 s, U64 count);  // the first `count` bytes
internal String8 str8_postfix(String8 s, U64 count); // the last `count` bytes
internal String8 str8_skip(String8 s, U64 count);    // remove `count` bytes from the start
internal String8 str8_chop(String8 s, U64 count);    // remove `count` bytes from the end

////////////////////////////////
//~ fp: Comparison & Search

typedef U32 StringMatchFlags;
enum {
  StringMatchFlag_CaseInsensitive = (1 << 0),
};

internal B32 str8_match(String8 a, String8 b, StringMatchFlags flags);
internal B32 str8_starts_with(String8 s, String8 prefix, StringMatchFlags flags);
internal B32 str8_ends_with(String8 s, String8 postfix, StringMatchFlags flags);

// The position of the first `needle` at `start_pos` or after it. The result is
// haystack.size when `haystack` does not contain `needle`.
internal U64 str8_find_substr(String8 haystack, String8 needle, U64 start_pos, StringMatchFlags flags);

////////////////////////////////
//~ fp: Arena Construction

// The format functions use the copy of stb_sprintf at base/stb_sprintf.h. That
// copy has one change: %S writes a String8, as in
// push_str8f(arena, "%S/%S", dir, name). The cost of that change is that the
// compiler cannot examine a format string here.
internal String8 push_str8_copy(Arena* arena, String8 s);
internal String8 push_str8_cat(Arena* arena, String8 a, String8 b);
internal String8 push_str8fv(Arena* arena, char* fmt, va_list args);
internal String8 push_str8f(Arena* arena, char* fmt, ...);

////////////////////////////////
//~ fp: String Lists
//
// A string list takes the place of a string builder. Push each view, or each
// copy, as a node. Then join the list one time, into one allocation. A node
// goes on the arena. A push does not copy the string itself.

typedef struct String8Node String8Node;
struct String8Node {
  String8Node* next;
  String8 string;
};

typedef struct {
  String8Node* first;
  String8Node* last;
  U64 node_count;
  U64 total_size;
} String8List;

typedef struct {
  String8 pre;
  String8 sep;
  String8 post;
} StringJoin;

internal void str8_list_push(Arena* arena, String8List* list, String8 s);
internal String8 str8_list_join(Arena* arena, String8List list, StringJoin* optional_join);

// Divide `s` at each of the given bytes. Two separators that follow each other
// give no empty part. Each string in the result is a view into `s`, and not a
// copy.
internal String8List str8_split(Arena* arena, String8 s, U8* split_bytes, U64 split_byte_count);
