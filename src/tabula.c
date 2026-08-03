#include "base/core.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/os.h"
#include "base/print.h"
#include "tabula.h"

////////////////////////////////
//~ fp: Lexer
//
// Token stream over the source. Atoms (bare words) cover both identifiers and
// numbers -- the parser decides which by trying to read the text as a number.

typedef U32 TB_TokenKind;
enum {
  TB_TokenKind_EOF, // zero, so a zeroed token reads as end-of-input
  TB_TokenKind_Atom,
  TB_TokenKind_String,
  TB_TokenKind_LBrace,
  TB_TokenKind_RBrace,
  TB_TokenKind_LBracket,
  TB_TokenKind_RBracket,
  TB_TokenKind_Equals,
};

typedef struct {
  TB_TokenKind kind;
  String8 text; // atom text, or string contents with quotes stripped
  U64 offset;
} TB_Token;

typedef struct {
  Arena* arena;
  String8 source;
  U64 at;         // lex cursor
  TB_Token token; // one-token lookahead
  U64 depth;      // object/list nesting, to bound recursion
  TB_ErrorList errors;
} TB_Parser;

#define TB_MAX_DEPTH 256

internal void tb__line_col_from_offset(String8 source, U64 offset, U64* out_line, U64* out_col) {
  U64 line = 1;
  U64 col = 1;
  offset = ClampTop(offset, source.size);
  for(U64 idx = 0; idx < offset; idx += 1) {
    if(source.str[idx] == '\n') { line += 1; col = 1; }
    else { col += 1; }
  }
  *out_line = line;
  *out_col = col;
}

internal void tb__errorf(TB_Parser* parser, U64 offset, char* fmt, ...) {
  TB_Error* error = push_array(parser->arena, TB_Error, 1);
  va_list args;
  va_start(args, fmt);
  error->message = push_str8fv(parser->arena, fmt, args);
  va_end(args);
  error->src_offset = offset;
  tb__line_col_from_offset(parser->source, offset, &error->line, &error->column);
  SLLQueuePush(parser->errors.first, parser->errors.last, error);
  parser->errors.count += 1;
}

internal B32 tb__is_whitespace(U8 c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

internal B32 tb__is_atom_terminator(U8 c) {
  return tb__is_whitespace(c) || c == '{' || c == '}' || c == '[' || c == ']' ||
         c == '=' || c == '"' || c == '#';
}

internal TB_Token tb__lex_token(TB_Parser* parser) {
  String8 src = parser->source;

  // skip whitespace and '#' line comments
  for(;;) {
    while(parser->at < src.size && tb__is_whitespace(src.str[parser->at])) { parser->at += 1; }
    if(parser->at < src.size && src.str[parser->at] == '#') {
      while(parser->at < src.size && src.str[parser->at] != '\n') { parser->at += 1; }
      continue;
    }
    break;
  }

  TB_Token token = {0};
  token.offset = parser->at;
  if(parser->at >= src.size) { return token; } // EOF

  U8 c = src.str[parser->at];
  switch(c) {
    case '{': { token.kind = TB_TokenKind_LBrace; parser->at += 1; } break;
    case '}': { token.kind = TB_TokenKind_RBrace; parser->at += 1; } break;
    case '[': { token.kind = TB_TokenKind_LBracket; parser->at += 1; } break;
    case ']': { token.kind = TB_TokenKind_RBracket; parser->at += 1; } break;
    case '=': { token.kind = TB_TokenKind_Equals; parser->at += 1; } break;

    case '"': {
      token.kind = TB_TokenKind_String;
      U64 contents_first = parser->at + 1;
      U64 close = contents_first;
      while(close < src.size && src.str[close] != '"') { close += 1; }
      token.text = str8_substr(src, contents_first, close);
      if(close < src.size) {
        parser->at = close + 1;
      } else {
        tb__errorf(parser, token.offset, "unterminated string");
        parser->at = close;
      }
    } break;

    default: {
      token.kind = TB_TokenKind_Atom;
      U64 first = parser->at;
      while(parser->at < src.size && !tb__is_atom_terminator(src.str[parser->at])) { parser->at += 1; }
      token.text = str8_substr(src, first, parser->at);
    } break;
  }
  return token;
}

internal void tb__advance(TB_Parser* parser) {
  parser->token = tb__lex_token(parser);
}

////////////////////////////////
//~ fp: Number Recognition
//
// '-'? digits ('.' digits)? and nothing else. Anything that doesn't fully
// match stays an identifier ("1.2.3", "2x", "-", ...).

internal B32 tb__f32_from_atom(String8 text, F32* out) {
  U64 at = 0;
  B32 negative = 0;
  if(at < text.size && text.str[at] == '-') { negative = 1; at += 1; }

  U64 int_digits = 0;
  F64 value = 0;
  while(at < text.size && '0' <= text.str[at] && text.str[at] <= '9') {
    value = value * 10.0 + (text.str[at] - '0');
    at += 1;
    int_digits += 1;
  }
  if(int_digits == 0) { return 0; }

  if(at < text.size && text.str[at] == '.') {
    at += 1;
    U64 frac_digits = 0;
    F64 scale = 0.1;
    while(at < text.size && '0' <= text.str[at] && text.str[at] <= '9') {
      value += (text.str[at] - '0') * scale;
      scale *= 0.1;
      at += 1;
      frac_digits += 1;
    }
    if(frac_digits == 0) { return 0; }
  }

  if(at != text.size) { return 0; }
  *out = (F32)(negative ? -value : value);
  return 1;
}

////////////////////////////////
//~ fp: Parser
//
// Recursive descent that never fails hard: every wrong token produces an error
// plus a recovery that provably advances, so parsing always terminates and the
// tree keeps everything that did parse.

internal void tb__parse_members(TB_Parser* parser, TB_Value* object, B32 is_root);

internal void tb__parse_value(TB_Parser* parser, TB_Value* out) {
  TB_Token token = parser->token;
  out->src_offset = token.offset;

  switch(token.kind) {
    case TB_TokenKind_Atom: {
      tb__advance(parser);
      F32 number = 0;
      if(tb__f32_from_atom(token.text, &number)) {
        out->kind = TB_ValueKind_Number;
        out->number = number;
      } else {
        out->kind = TB_ValueKind_Identifier;
      }
      out->text = token.text;
    } break;

    case TB_TokenKind_String: {
      tb__advance(parser);
      out->kind = TB_ValueKind_String;
      out->text = token.text;
    } break;

    case TB_TokenKind_LBrace: {
      tb__advance(parser);
      out->kind = TB_ValueKind_Object;
      if(parser->depth < TB_MAX_DEPTH) {
        parser->depth += 1;
        tb__parse_members(parser, out, 0);
        parser->depth -= 1;
      } else {
        tb__errorf(parser, token.offset, "nesting deeper than %d levels", TB_MAX_DEPTH);
      }
    } break;

    case TB_TokenKind_LBracket: {
      tb__advance(parser);
      out->kind = TB_ValueKind_List;
      if(parser->depth < TB_MAX_DEPTH) {
        parser->depth += 1;
        for(;;) {
          TB_TokenKind k = parser->token.kind;
          if(k == TB_TokenKind_RBracket) { tb__advance(parser); break; }
          if(k == TB_TokenKind_EOF) {
            tb__errorf(parser, token.offset, "unterminated list");
            break;
          }
          if(k == TB_TokenKind_Equals || k == TB_TokenKind_RBrace) {
            tb__errorf(parser, parser->token.offset, "unexpected token in list");
            tb__advance(parser);
            continue;
          }
          TB_Value* element = push_array(parser->arena, TB_Value, 1);
          tb__parse_value(parser, element);
          SLLQueuePush(out->first, out->last, element);
          out->count += 1;
        }
        parser->depth -= 1;
      } else {
        tb__errorf(parser, token.offset, "nesting deeper than %d levels", TB_MAX_DEPTH);
      }
    } break;

    default: {
      // Equals / RBrace / RBracket / EOF in value position: report, leave the
      // value nil, and do NOT consume -- the caller knows how to handle these
      tb__errorf(parser, token.offset, "expected a value");
    } break;
  }
}

internal void tb__parse_members(TB_Parser* parser, TB_Value* object, B32 is_root) {
  U64 open_offset = parser->token.offset;
  for(;;) {
    TB_Token token = parser->token;
    switch(token.kind) {
      case TB_TokenKind_EOF: {
        if(!is_root) { tb__errorf(parser, open_offset, "unterminated object"); }
        return;
      }

      case TB_TokenKind_RBrace: {
        if(is_root) {
          tb__errorf(parser, token.offset, "stray '}'");
          tb__advance(parser);
          continue;
        }
        tb__advance(parser);
        return;
      }

      case TB_TokenKind_Atom:
      case TB_TokenKind_String: {
        tb__advance(parser);
        if(parser->token.kind == TB_TokenKind_Equals) {
          tb__advance(parser);
          TB_Node* node = push_array(parser->arena, TB_Node, 1);
          node->key = token.text;
          node->src_offset = token.offset;
          tb__parse_value(parser, &node->value);
          DLLPushBack(object->first_member, object->last_member, node);
          object->member_count += 1;
        } else {
          // the offending token is not consumed -- it may be the next key
          tb__errorf(parser, token.offset, "expected '=' after key '%S'", token.text);
        }
      } break;

      case TB_TokenKind_Equals: {
        tb__errorf(parser, token.offset, "expected a key before '='");
        tb__advance(parser);
        // swallow the orphaned value, if one follows, to stay aligned
        TB_TokenKind k = parser->token.kind;
        if(k == TB_TokenKind_Atom || k == TB_TokenKind_String ||
           k == TB_TokenKind_LBrace || k == TB_TokenKind_LBracket) {
          TB_Value discard = {0};
          tb__parse_value(parser, &discard);
        }
      } break;

      case TB_TokenKind_LBrace:
      case TB_TokenKind_LBracket: {
        tb__errorf(parser, token.offset, "expected 'key =' before value");
        TB_Value discard = {0};
        tb__parse_value(parser, &discard); // consume the whole subtree
      } break;

      case TB_TokenKind_RBracket: {
        tb__errorf(parser, token.offset, "stray ']'");
        tb__advance(parser);
      } break;
    }
  }
}

internal TB_ParseResult tb_parse(Arena* arena, String8 source) {
  TB_Parser parser = {0};
  parser.arena = arena;
  parser.source = source;
  tb__advance(&parser);

  TB_Value* root = push_array(arena, TB_Value, 1);
  root->kind = TB_ValueKind_Object;
  tb__parse_members(&parser, root, 1);

  TB_ParseResult result = {root, parser.errors};
  return result;
}

internal TB_Value* tb_parse_file_and_report(Arena* arena, String8 path) {
  TB_Value* result = &tb_nil_value;
  // the tree holds views into the source text, so both live on the same arena
  B32 read_ok = 0;
  String8 source = os_read_entire_file(arena, path, &read_ok);
  if(!read_ok) {
    eprintf_str8("%S: cannot read file\n", path);
  } else {
    TB_ParseResult parse = tb_parse(arena, source);
    tb_print_errors(path, parse.errors);
    result = parse.root;
  }
  return result;
}

////////////////////////////////
//~ fp: Accessors

internal B32 tb_value_is_nil(TB_Value* value) {
  return value == 0 || value->kind == TB_ValueKind_Nil;
}

internal TB_Value* tb_get(TB_Value* object, String8 key) {
  if(object == 0) { object = &tb_nil_value; }
  TB_Value* result = &tb_nil_value;
  if(object->kind == TB_ValueKind_Object) {
    for(TB_Node* node = object->first_member; node != 0; node = node->next) {
      if(str8_match(node->key, key, 0)) {
        result = &node->value;
        break;
      }
    }
  }
  return result;
}

internal B32 tb_has(TB_Value* object, String8 key) {
  return tb_get(object, key) != &tb_nil_value;
}

internal String8 tb_str8_from_value(TB_Value* value, String8 fallback) {
  String8 result = fallback;
  if(value != 0 && (value->kind == TB_ValueKind_String || value->kind == TB_ValueKind_Identifier)) {
    result = value->text;
  }
  return result;
}

internal F32 tb_num_from_value(TB_Value* value, F32 fallback) {
  F32 result = fallback;
  if(value != 0 && value->kind == TB_ValueKind_Number) {
    result = value->number;
  }
  return result;
}

internal String8 tb_get_str8(TB_Value* object, String8 key, String8 fallback) {
  return tb_str8_from_value(tb_get(object, key), fallback);
}

internal F32 tb_get_num(TB_Value* object, String8 key, F32 fallback) {
  return tb_num_from_value(tb_get(object, key), fallback);
}

////////////////////////////////
//~ fp: Error Reporting

internal String8 tb_str8_from_error(Arena* arena, String8 label, TB_Error* error) {
  return push_str8f(arena, "%S:%llu:%llu: %S", label,
                    (unsigned long long)error->line, (unsigned long long)error->column,
                    error->message);
}

internal void tb_print_errors(String8 label, TB_ErrorList errors) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  for(TB_Error* error = errors.first; error != 0; error = error->next) {
    eprintf_str8("%S\n", tb_str8_from_error(scratch.arena, label, error));
  }
  arena_release_scratch(scratch);
}
