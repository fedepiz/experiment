#pragma once

#include <stdarg.h>

#include "base/core.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: String8
//
// Pointer + length, no null terminator, no ownership. A String8 is a view by
// default: slicing and splitting re-point into existing memory, and only the
// push_* functions allocate. Arena-constructed strings do get one null byte
// past `size`, so their .str can be handed to C APIs directly.

typedef struct {
  U8* str;
  U64 size;
} String8;

//- fp: constructors
internal String8 str8(U8* str, U64 size);
internal String8 str8_cstring(char* cstr);
#define str8_lit(s) str8((U8*)(s), sizeof(s) - 1)

//- fp: printf bridge -- printf("%.*s", str8_varg(s))
#define str8_varg(s) (int)(s).size, (s).str

////////////////////////////////
//~ fp: Slicing (no allocation)

// substr clamps both bounds; first is inclusive, opl ("one past last") is not.
internal String8 str8_substr(String8 s, U64 first, U64 opl);
internal String8 str8_prefix(String8 s, U64 count); // first count bytes
internal String8 str8_postfix(String8 s, U64 count); // last count bytes
internal String8 str8_skip(String8 s, U64 count); // drop count from the front
internal String8 str8_chop(String8 s, U64 count); // drop count from the back

////////////////////////////////
//~ fp: Comparison & Search

typedef U32 StringMatchFlags;
enum {
  StringMatchFlag_CaseInsensitive = (1 << 0),
};

internal B32 str8_match(String8 a, String8 b, StringMatchFlags flags);
internal B32 str8_starts_with(String8 s, String8 prefix, StringMatchFlags flags);
internal B32 str8_ends_with(String8 s, String8 postfix, StringMatchFlags flags);

// returns the position of the first occurrence at or after start_pos, or
// haystack.size when the needle does not occur
internal U64 str8_find_substr(String8 haystack, String8 needle, U64 start_pos, StringMatchFlags flags);

////////////////////////////////
//~ fp: Arena Construction

// formatting goes through our stb_sprintf (base/stb_sprintf.h), patched with
// %S for String8: push_str8f(arena, "%S/%S", dir, name). The price of custom
// specifiers is that the compiler's printf format checking can't apply here.
internal String8 push_str8_copy(Arena* arena, String8 s);
internal String8 push_str8_cat(Arena* arena, String8 a, String8 b);
internal String8 push_str8fv(Arena* arena, char* fmt, va_list args);
internal String8 push_str8f(Arena* arena, char* fmt, ...);

////////////////////////////////
//~ fp: String Lists
//
// The idiom that replaces string builders: push views (or copies) as nodes,
// join once at the end into a single allocation. Nodes go on the arena; the
// strings themselves are NOT copied on push.

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

internal void    str8_list_push(Arena* arena, String8List* list, String8 s);
internal String8 str8_list_join(Arena* arena, String8List list, StringJoin* optional_join);

// splits on any of the given bytes; consecutive separators produce no empty
// pieces. The resulting strings are views into `s`, not copies.
internal String8List str8_split(Arena* arena, String8 s, U8* split_bytes, U64 split_byte_count);
