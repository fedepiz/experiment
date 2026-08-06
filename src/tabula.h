#pragma once

#include "base/core.h"
#include "base/strings.h"

// The 'tabula' format is a data format similar to JSON or Metadesk, but that uses the syntax of paradox scripting languages.
// "Objects" are delimited via { }, and contains lists of key-value pairs of the form key = value. values can be identifiers, string literals, numbers, objects or
// lists (delimited by [ ], content space-separated)
//
// Parsing a file yields a signle synthetic Node, whose members are the top-level key-value declarations

////////////////////////////////
//~ fp: Values
//
// Fat struct: every field exists on every value, the kind just says which ones
// mean anything. Unused fields are zero, and the zero value is the nil value
// (kind == TB_ValueKind_Nil), so a zeroed TB_Value is always safe to read.

typedef U32 TB_ValueKind;
enum {
  TB_ValueKind_Nil, // zero -> ZII gives nil for free
  TB_ValueKind_Identifier,
  TB_ValueKind_String,
  TB_ValueKind_Number,
  TB_ValueKind_Object,
  TB_ValueKind_List,
};

typedef struct TB_Node TB_Node;
typedef struct TB_Value TB_Value;
struct TB_Value {
  TB_Value* next; // sibling when this value is an element of a List

  TB_ValueKind kind;

  // source text, always set for leaf kinds: the identifier itself, the string
  // contents (quotes stripped), or the number's exact spelling
  String8 text;

  // parsed numeric value (kind == Number); `text` still holds the spelling, so
  // "1.50" round-trips as written while `number` is ready for arithmetic
  F32 number;

  // elements (kind == List)
  TB_Value* first;
  TB_Value* last;
  U64 count;

  // members (kind == Object)
  TB_Node* first_member;
  TB_Node* last_member;
  U64 member_count;

  // byte offset of this value's first character in the source, for diagnostics
  U64 src_offset;
};

////////////////////////////////
//~ fp: Nodes
//
// A node is one `key = value` pair. The value is embedded, not pointed to:
// no allocation for the common case, and a zeroed node carries a nil value.
// The root node returned by parsing is synthetic: empty key, Object value
// holding the top-level declarations as members.

struct TB_Node {
  TB_Node* next;
  TB_Node* prev;

  String8 key;
  TB_Value value;

  U64 src_offset;
};

////////////////////////////////
//~ fp: Errors
//
// Parsing never fails hard: it always yields a (possibly nil) tree, plus a
// list of everything it did not like. Nodes go on the same arena as the tree.

typedef struct TB_Error TB_Error;
struct TB_Error {
  TB_Error* next;
  String8 message;
  U64 src_offset;
  U64 line;   // 1-based
  U64 column; // 1-based
};

typedef struct {
  TB_Error* first;
  TB_Error* last;
  U64 count;
} TB_ErrorList;

////////////////////////////////
//~ fp: Nil
//
// The shared sentinel every miss resolves to. Zeroed == nil (ZII), so this
// needs no initialization. Its member list is empty, so lookups on it return
// it again: chains of gets never crash, absence just propagates. Read-only by
// convention -- nothing may ever write through a nil.

global TB_Value tb_nil_value;

internal B32 tb_value_is_nil(TB_Value* value);

////////////////////////////////
//~ fp: Parse Result

typedef struct {
  TB_Value* root; // synthetic Object value; check errors.count for problems
  TB_ErrorList errors;
} TB_ParseResult;

internal TB_ParseResult tb_parse(Arena* arena, String8 source);
// Common case helper
internal TB_Value* tb_parse_file_and_report(Arena* arena, String8 path);

////////////////////////////////
//~ fp: Accessors
//
// All pointer-taking functions normalize 0 to &tb_nil_value on entry and never
// return 0 -- a miss (absent key, kind mismatch, lookup inside a non-object)
// yields nil, which further lookups pass through unchanged:
//
//   F32 g = tb_get_num(tb_get(cfg, str8_lit("physics")), str8_lit("gravity"), 9.81f);
//
// works no matter which part of the chain is missing.

internal TB_Value* tb_get(TB_Value* object, String8 key);
internal B32       tb_has(TB_Value* object, String8 key);

//- fp: keyed reads with fallbacks
internal String8 tb_get_str8(TB_Value* object, String8 key, String8 fallback); // String or Identifier
internal F32     tb_get_num(TB_Value* object, String8 key, F32 fallback);

//- fp: coercions on a value in hand (list elements, tb_get results)
internal String8 tb_str8_from_value(TB_Value* value, String8 fallback);
internal F32     tb_num_from_value(TB_Value* value, F32 fallback);

////////////////////////////////
//~ fp: Builder
//
// Programmatic construction of trees, for producers (extractions, reports)
// whose consumers read them like parsed files. Total in the module's style:
// adding to a nil or non-Object/non-List target no-ops, and functions that
// return a value return nil then. Strings are stored as views -- callers
// keep them alive as long as the tree (push them on the same arena).
// Numbers get a "%g" text spelling, so they read back as strings too.

internal TB_Value* tb_build_object(Arena* arena); // a standalone Object value
internal TB_Value* tb_build_list(Arena* arena);   // a standalone List value

//- fp: object members; tb_add returns the new member's nil value for the
//  caller to fill, the wrappers fill it themselves
internal TB_Value* tb_add(Arena* arena, TB_Value* object, String8 key);
internal void      tb_add_str8(Arena* arena, TB_Value* object, String8 key, String8 value);
internal void      tb_add_num(Arena* arena, TB_Value* object, String8 key, F32 value);
internal TB_Value* tb_add_object(Arena* arena, TB_Value* object, String8 key);
internal TB_Value* tb_add_list(Arena* arena, TB_Value* object, String8 key);

//- fp: list elements, same protocol
internal TB_Value* tb_list_push(Arena* arena, TB_Value* list);
internal void      tb_list_push_str8(Arena* arena, TB_Value* list, String8 value);
internal void      tb_list_push_num(Arena* arena, TB_Value* list, F32 value);
internal TB_Value* tb_list_push_object(Arena* arena, TB_Value* list);

////////////////////////////////
//~ fp: Error Reporting
//
// label names the source (a path, "<stdin>", ...); TB_Error doesn't know it
// because tb_parse only ever sees text. The format is gcc-style
// "label:line:col: message", which editors and terminals know how to jump to.

internal String8 tb_str8_from_error(Arena* arena, String8 label, TB_Error* error);
internal void    tb_print_errors(String8 label, TB_ErrorList errors); // to stderr

