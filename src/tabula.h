#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/strings.h"

// The tabula format is a data format. It is near to JSON and to Metadesk, and
// it uses the syntax of the script languages of Paradox.
//
// A pair of braces holds an object, and an object holds a set of `key = value`
// pairs. A key is an identifier or a string between quotation marks. A value
// is an identifier, a string between quotation marks, a number, an object, or
// a list. A pair of brackets holds a list, and a space separates two elements
// of a list. A number sign starts a comment, which ends at the end of the
// line.
//
// A parse of a file gives a root object that the parser makes. The members of
// that object are the `key = value` pairs at the top level of the file.

////////////////////////////////
//~ fp: Values
//
// Each value carries each member of this struct. `kind` says which of them
// hold a value. A member that the kind does not use is 0. The zero value is
// the nil value, whose kind is TB_ValueKind_Nil, so a read of a TB_Value that
// holds zeros is always correct.

typedef U32 TB_ValueKind;
enum {
  TB_ValueKind_Nil, // Its value is 0, so a struct of zeros is the nil value (ZII).
  TB_ValueKind_Identifier,
  TB_ValueKind_String,
  TB_ValueKind_Number,
  TB_ValueKind_Object,
  TB_ValueKind_List,
};

typedef struct TB_Node TB_Node;
typedef struct TB_Value TB_Value;
struct TB_Value {
  TB_Value* next; // the next element, where this value is an element of a list

  TB_ValueKind kind;

  // The text of the source. Each kind that holds no other value sets it: the
  // identifier itself, the contents of the string without the quotation marks,
  // or the exact text of the number.
  String8 text;

  // The value of the number, where the kind is Number. `text` still holds the
  // text of the source, so "1.50" writes again as it was, and `number` is
  // ready for arithmetic.
  F32 number;

  // the elements, where the kind is List
  TB_Value* first;
  TB_Value* last;
  U64 count;

  // the members, where the kind is Object
  TB_Node* first_member;
  TB_Node* last_member;
  U64 member_count;

  // The offset in bytes of the first character of this value in the source.
  // An error report uses it.
  U64 src_offset;
};

////////////////////////////////
//~ fp: Nodes
//
// A node is one `key = value` pair. The node holds the value itself, and not a
// pointer to it. The usual case therefore needs no allocation, and a node of
// zeros holds the nil value.
//
// The parser makes the root node. Its key is empty, and its value is an object
// whose members are the pairs at the top level of the file.

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
// A parse always gives a tree, which can be nil, and a list of each problem
// that the parser found. A parse does not stop the program. Each node of that
// list is on the arena of the tree.

typedef struct TB_Error TB_Error;
struct TB_Error {
  TB_Error* next;
  String8 message;
  U64 src_offset;
  U64 line;   // The first line is 1.
  U64 column; // The first column is 1.
};

typedef struct {
  TB_Error* first;
  TB_Error* last;
  U64 count;
} TB_ErrorList;

////////////////////////////////
//~ fp: Nil
//
// The one object that each read of an absent value gives. A struct of zeros is
// the nil value (ZII), so this object needs no initialization.
//
// Its list of members is empty, so a read of a member of it gives it again. A
// chain of reads therefore cannot fail, and the absence of a value passes
// through that chain. This object is read-only by convention: no code may
// write through a nil value.

global TB_Value tb_nil_value;

internal B32 tb_value_is_nil(TB_Value* value);

////////////////////////////////
//~ fp: Parse Result

typedef struct {
  TB_Value* root; // The object that the parser makes. Read errors.count to find a problem.
  TB_ErrorList errors;
} TB_ParseResult;

internal TB_ParseResult tb_parse(Arena* arena, String8 source);
// the function for the usual case
internal TB_Value* tb_parse_file_and_report(Arena* arena, String8 path);

////////////////////////////////
//~ fp: Accessors
//
// Each function that takes a pointer changes a pointer of 0 into
// &tb_nil_value, and no such function gives 0 back. A read that finds nothing
// gives the nil value. A key that is absent, a kind that is different, and a
// read inside a value that is not an object are the three such cases. A later
// read of that nil value gives the nil value again:
//
//   F32 g = tb_get_num(tb_get(cfg, str8_lit("physics")), str8_lit("gravity"), 9.81f);
//
// That line is correct where any part of the chain is absent.

internal TB_Value* tb_get(TB_Value* object, String8 key);
internal B32       tb_has(TB_Value* object, String8 key);

//- fp: the reads by key, each with a fallback
internal String8 tb_get_str8(TB_Value* object, String8 key, String8 fallback); // a String value or an Identifier value
internal F32     tb_get_num(TB_Value* object, String8 key, F32 fallback);
// A list of [r g b a], which is how a color reads. A list of three elements
// gives an alpha of 1. A key that is absent, a kind that is different, and a
// list of less than three elements each give the whole fallback.
internal V4      tb_get_v4(TB_Value* object, String8 key, V4 fallback);

//- fp: the conversions of a value that the caller holds, such as an element of
//  a list, and a result of tb_get
internal String8 tb_str8_from_value(TB_Value* value, String8 fallback);
internal F32     tb_num_from_value(TB_Value* value, F32 fallback);

////////////////////////////////
//~ fp: Builder
//
// These functions build a tree from code. Use them for a producer, such as an
// extraction or a report, whose reader reads the result as it reads a file
// that the parser gave.
//
// Each function is total, as in the rest of this module. An add to a nil
// target, and an add to a target that is not an object and not a list, write
// nothing, and a function that gives a value then gives the nil value.
//
// A string goes into the tree as a view. The caller must keep the bytes of
// that string for the life of the tree, so push the string on the arena of the
// tree. A number takes a text in the format "%g", so a read of that number as
// a string is correct.

internal TB_Value* tb_build_object(Arena* arena); // an Object value that stands alone
internal TB_Value* tb_build_list(Arena* arena);   // a List value that stands alone

//- fp: The members of an object. tb_add gives the nil value of the new member,
//  and the caller writes it. Each function below tb_add writes that value
//  itself.
internal TB_Value* tb_add(Arena* arena, TB_Value* object, String8 key);
internal void      tb_add_str8(Arena* arena, TB_Value* object, String8 key, String8 value);
internal void      tb_add_num(Arena* arena, TB_Value* object, String8 key, F32 value);
internal TB_Value* tb_add_object(Arena* arena, TB_Value* object, String8 key);
internal TB_Value* tb_add_list(Arena* arena, TB_Value* object, String8 key);

//- fp: the elements of a list, with the same rules
internal TB_Value* tb_list_push(Arena* arena, TB_Value* list);
internal void      tb_list_push_str8(Arena* arena, TB_Value* list, String8 value);
internal void      tb_list_push_num(Arena* arena, TB_Value* list, F32 value);
internal TB_Value* tb_list_push_object(Arena* arena, TB_Value* list);

////////////////////////////////
//~ fp: Error Reporting
//
// `label` names the source, such as a path or "<stdin>". A TB_Error does not
// hold that name, because tb_parse reads text alone. The format is
// "label:line:col: message", which is the format of gcc. An editor and a
// terminal can find a position from a line in that format.

internal String8 tb_str8_from_error(Arena* arena, String8 label, TB_Error* error);
internal void    tb_print_errors(String8 label, TB_ErrorList errors); // it writes to stderr

