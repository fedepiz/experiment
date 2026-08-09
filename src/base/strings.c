#include "base/core.h"
#include "base/arena.h"
#include "base/strings.h"

// The copy of the formatter that this project holds. It supplies
// stbsp_vsnprintf, and it adds %S for a String8. STATIC keeps its symbols
// inside this translation unit, as with each other symbol here.
#define STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_STATIC
#include "base/stb_sprintf.h"

////////////////////////////////
//~ fp: Char Helpers

internal U8 u8_to_lower(U8 c) {
  return ('A' <= c && c <= 'Z') ? c + ('a' - 'A') : c;
}

////////////////////////////////
//~ fp: String8

internal String8 str8(U8* str, U64 size) {
  String8 result = {str, size};
  return result;
}

internal String8 str8_cstring(const char* cstr) {
  return str8((U8*)cstr, strlen(cstr));
}

////////////////////////////////
//~ fp: Slicing

internal String8 str8_substr(String8 s, U64 first, U64 opl) {
  opl = ClampTop(opl, s.size);
  first = ClampTop(first, opl);
  return str8(s.str + first, opl - first);
}

internal String8 str8_prefix(String8 s, U64 count) {
  return str8_substr(s, 0, count);
}

internal String8 str8_postfix(String8 s, U64 count) {
  count = ClampTop(count, s.size);
  return str8_substr(s, s.size - count, s.size);
}

internal String8 str8_skip(String8 s, U64 count) {
  return str8_substr(s, count, s.size);
}

internal String8 str8_chop(String8 s, U64 count) {
  count = ClampTop(count, s.size);
  return str8_substr(s, 0, s.size - count);
}

////////////////////////////////
//~ fp: Comparison & Search

internal B32 str8_match(String8 a, String8 b, StringMatchFlags flags) {
  B32 result = 0;
  if(a.size == b.size) {
    result = 1;
    for(U64 idx = 0; idx < a.size; idx += 1) {
      U8 ac = a.str[idx];
      U8 bc = b.str[idx];
      if(flags & StringMatchFlag_CaseInsensitive) {
        ac = u8_to_lower(ac);
        bc = u8_to_lower(bc);
      }
      if(ac != bc) { result = 0; break; }
    }
  }
  return result;
}

internal B32 str8_starts_with(String8 s, String8 prefix, StringMatchFlags flags) {
  return prefix.size <= s.size && str8_match(str8_prefix(s, prefix.size), prefix, flags);
}

internal B32 str8_ends_with(String8 s, String8 postfix, StringMatchFlags flags) {
  return postfix.size <= s.size && str8_match(str8_postfix(s, postfix.size), postfix, flags);
}

internal U64 str8_find_substr(String8 haystack, String8 needle, U64 start_pos, StringMatchFlags flags) {
  U64 result = haystack.size;
  if(needle.size <= haystack.size) {
    for(U64 idx = start_pos; idx <= haystack.size - needle.size; idx += 1) {
      if(str8_match(str8_substr(haystack, idx, idx + needle.size), needle, flags)) {
        result = idx;
        break;
      }
    }
  }
  return result;
}

////////////////////////////////
//~ fp: Arena Construction

internal String8 push_str8_copy(Arena* arena, String8 s) {
  String8 result;
  result.size = s.size;
  result.str = push_array_no_zero(arena, U8, s.size + 1);
  MemoryCopy(result.str, s.str, s.size);
  result.str[result.size] = 0;
  return result;
}

internal String8 push_str8_cat(Arena* arena, String8 a, String8 b) {
  String8 result;
  result.size = a.size + b.size;
  result.str = push_array_no_zero(arena, U8, result.size + 1);
  MemoryCopy(result.str, a.str, a.size);
  MemoryCopy(result.str + a.size, b.str, b.size);
  result.str[result.size] = 0;
  return result;
}

internal String8 push_str8fv(Arena* arena, char* fmt, va_list args) {
  // Measure the result first, then write the format into the arena.
  va_list args2;
  va_copy(args2, args);
  U64 size = (U64)stbsp_vsnprintf(0, 0, fmt, args);
  String8 result;
  result.size = size;
  result.str = push_array_no_zero(arena, U8, (U64)size + 1);
  stbsp_vsnprintf((char*)result.str, (int)size + 1, fmt, args2);
  va_end(args2);
  return result;
}

internal String8 push_str8f(Arena* arena, char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = push_str8fv(arena, fmt, args);
  va_end(args);
  return result;
}

////////////////////////////////
//~ fp: String Lists

internal void str8_list_push(Arena* arena, String8List* list, String8 s) {
  String8Node* node = push_array(arena, String8Node, 1);
  node->string = s;
  SLLQueuePush(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += s.size;
}

internal String8 str8_list_join(Arena* arena, String8List list, StringJoin* optional_join) {
  StringJoin join = {0};
  if(optional_join != 0) { join = *optional_join; }

  U64 sep_count = (list.node_count > 0) ? list.node_count - 1 : 0;
  String8 result;
  result.size = join.pre.size + list.total_size + sep_count * join.sep.size + join.post.size;
  result.str = push_array_no_zero(arena, U8, result.size + 1);

  // `pre`, `sep` and `post` are nil when the caller does not give them. Skip
  // the copy for a nil string: a move from a nil pointer is undefined, and it
  // is undefined for 0 bytes too.
  U8* at = result.str;
  if(join.pre.size) { MemoryCopy(at, join.pre.str, join.pre.size); }
  at += join.pre.size;
  for(String8Node* node = list.first; node != 0; node = node->next) {
    MemoryCopy(at, node->string.str, node->string.size);
    at += node->string.size;
    if(node->next != 0 && join.sep.size) {
      MemoryCopy(at, join.sep.str, join.sep.size);
      at += join.sep.size;
    }
  }
  if(join.post.size) { MemoryCopy(at, join.post.str, join.post.size); }
  at += join.post.size;
  *at = 0;
  Assert((U64)(at - result.str) == result.size);
  return result;
}

internal String8List str8_split(Arena* arena, String8 s, U8* split_bytes, U64 split_byte_count) {
  String8List result = {0};
  U64 piece_first = 0;
  for(U64 idx = 0; idx <= s.size; idx += 1) {
    B32 is_split = (idx == s.size);
    for(U64 s_idx = 0; !is_split && s_idx < split_byte_count; s_idx += 1) {
      is_split = (s.str[idx] == split_bytes[s_idx]);
    }
    if(is_split) {
      if(idx > piece_first) {
        str8_list_push(arena, &result, str8_substr(s, piece_first, idx));
      }
      piece_first = idx + 1;
    }
  }
  return result;
}
