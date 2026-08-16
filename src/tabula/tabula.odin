package tabula

import "core:fmt"
import "core:mem"
import "core:os"
import "core:testing"

import "../chunk"
import "../geo"

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
// the nil value, whose kind is .Nil, so a read of a Value that holds zeros is
// always correct.

Value_Kind :: enum {
	Nil, // Its value is 0, so a struct of zeros is the nil value (ZII).
	Identifier,
	String,
	Number,
	Object,
	List,
}

Value :: struct {
	next:         ^Value, // the next element, where this value is an element of a list
	kind:         Value_Kind,

	// The text of the source. Each kind that holds no other value sets it: the
	// identifier itself, the contents of the string without the quotation marks,
	// or the exact text of the number.
	text:         string,

	// The value of the number, where the kind is Number. `text` still holds the
	// text of the source, so "1.50" writes again as it was, and `number` is
	// ready for arithmetic.
	number:       f32,

	// the elements, where the kind is List
	first:        ^Value,
	last:         ^Value,
	count:        int,

	// the members, where the kind is Object
	first_member: ^Node,
	last_member:  ^Node,
	member_count: int,

	// The offset in bytes of the first character of this value in the source.
	// An error report uses it.
	src_offset:   int,
}

////////////////////////////////
//~ fp: Nodes
//
// A node is one `key = value` pair. The node holds the value itself, and not a
// pointer to it. The usual case therefore needs no allocation, and a node of
// zeros holds the nil value.
//
// The parser makes the root node. Its key is empty, and its value is an object
// whose members are the pairs at the top level of the file.

Node :: struct {
	next:       ^Node,
	prev:       ^Node,
	key:        string,
	value:      Value,
	src_offset: int,
}

////////////////////////////////
//~ fp: Errors
//
// A parse always gives a tree, which can be nil, and a list of each problem
// that the parser found. A parse does not stop the program. Each node of that
// list is on the allocator of the tree.

Error :: struct {
	message:    string,
	src_offset: int,
	line:       int, // The first line is 1.
	column:     int, // The first column is 1.
}

@(private)
ERROR_CHUNK_CAP :: 16
Error_List :: chunk.List(Error, ERROR_CHUNK_CAP)

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

nil_value: Value

value_is_nil :: proc(value: ^Value) -> bool {
	return value == nil || value.kind == .Nil
}

////////////////////////////////
//~ fp: Lexer
//
// The stream of tokens of the source. An atom is a word with no quotation
// marks, and it covers an identifier and a number. The parser reads the text
// of an atom as a number, and the result of that read decides which of the two
// the atom is.

@(private)
Token_Kind :: enum {
	EOF, // Its value is 0, so a token of zeros is the end of the input.
	Atom,
	String,
	L_Brace,
	R_Brace,
	L_Bracket,
	R_Bracket,
	Equals,
}

@(private)
Token :: struct {
	kind:   Token_Kind,
	text:   string, // the text of an atom, or the contents of a string without the quotation marks
	offset: int,
}

@(private)
Parser :: struct {
	allocator: mem.Allocator,
	source:    string,
	at:        int, // the position of the lexer in the source
	token:     Token, // the next token, which the parser reads before it needs it
	depth:     int, // the objects and the lists inside each other, which limits the recursion
	errors:    Error_List,
}

@(private)
MAX_DEPTH :: 256

@(private)
line_col_from_offset :: proc(source: string, offset: int) -> (line, col: int) {
	line = 1
	col = 1
	offset := min(offset, len(source))
	for idx in 0 ..< offset {
		if source[idx] == '\n' {line += 1; col = 1} else {col += 1}
	}
	return
}

@(private)
errorf :: proc(parser: ^Parser, offset: int, format: string, args: ..any) {
	error: Error
	error.message = fmt.aprintf(format, ..args, allocator = parser.allocator)
	error.src_offset = offset
	error.line, error.column = line_col_from_offset(parser.source, offset)
	chunk.push(&parser.errors, error, parser.allocator)
}

@(private)
is_whitespace :: proc(c: u8) -> bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n'
}

@(private)
is_atom_terminator :: proc(c: u8) -> bool {
	return(
		is_whitespace(c) ||
		c == '{' ||
		c == '}' ||
		c == '[' ||
		c == ']' ||
		c == '=' ||
		c == '"' ||
		c == '#' \
	)
}

@(private)
lex_token :: proc(parser: ^Parser) -> Token {
	src := parser.source

	// Step over each space, and over each comment, which starts at a number
	// sign and ends at the end of its line.
	for {
		for parser.at < len(src) && is_whitespace(src[parser.at]) {parser.at += 1}
		if parser.at < len(src) && src[parser.at] == '#' {
			for parser.at < len(src) && src[parser.at] != '\n' {parser.at += 1}
			continue
		}
		break
	}

	token: Token
	token.offset = parser.at
	if parser.at >= len(src) {return token} 	// the end of the input

	c := src[parser.at]
	switch c {
	case '{':
		token.kind = .L_Brace; parser.at += 1
	case '}':
		token.kind = .R_Brace; parser.at += 1
	case '[':
		token.kind = .L_Bracket; parser.at += 1
	case ']':
		token.kind = .R_Bracket; parser.at += 1
	case '=':
		token.kind = .Equals; parser.at += 1

	case '"':
		token.kind = .String
		contents_first := parser.at + 1
		close := contents_first
		for close < len(src) && src[close] != '"' {close += 1}
		token.text = src[contents_first:close]
		if close < len(src) {
			parser.at = close + 1
		} else {
			errorf(parser, token.offset, "unterminated string")
			parser.at = close
		}

	case:
		token.kind = .Atom
		first := parser.at
		for parser.at < len(src) && !is_atom_terminator(src[parser.at]) {parser.at += 1}
		token.text = src[first:parser.at]
	}
	return token
}

@(private)
advance :: proc(parser: ^Parser) {
	parser.token = lex_token(parser)
}

////////////////////////////////
//~ fp: Number Recognition
//
// A number is an optional minus sign, then digits, then an optional point with
// more digits after it, and nothing more. A text that does not match that
// pattern in full stays an identifier, such as "1.2.3", "2x" and "-".

@(private)
f32_from_atom :: proc(text: string) -> (f32, bool) {
	at := 0
	negative := false
	if at < len(text) && text[at] == '-' {negative = true; at += 1}

	int_digits := 0
	value: f64 = 0
	for at < len(text) && '0' <= text[at] && text[at] <= '9' {
		value = value * 10.0 + f64(text[at] - '0')
		at += 1
		int_digits += 1
	}
	if int_digits == 0 {return 0, false}

	if at < len(text) && text[at] == '.' {
		at += 1
		frac_digits := 0
		scale: f64 = 0.1
		for at < len(text) && '0' <= text[at] && text[at] <= '9' {
			value += f64(text[at] - '0') * scale
			scale *= 0.1
			at += 1
			frac_digits += 1
		}
		if frac_digits == 0 {return 0, false}
	}

	if at != len(text) {return 0, false}
	return f32(negative ? -value : value), true
}

////////////////////////////////
//~ fp: Parser
//
// The parser goes down through the grammar with one function for each rule,
// and it does not stop the program. Each token that is wrong makes an error
// and a step that always moves forward. A parse therefore always ends, and the
// tree holds each part that the parser read.

@(private)
node_push_back :: proc(object: ^Value, node: ^Node) {
	if object.first_member == nil {
		object.first_member = node
		object.last_member = node
	} else {
		node.prev = object.last_member
		object.last_member.next = node
		object.last_member = node
	}
	object.member_count += 1
}

@(private)
parse_value :: proc(parser: ^Parser, out: ^Value) {
	token := parser.token
	out.src_offset = token.offset

	switch token.kind {
	case .Atom:
		advance(parser)
		if number, ok := f32_from_atom(token.text); ok {
			out.kind = .Number
			out.number = number
		} else {
			out.kind = .Identifier
		}
		out.text = token.text

	case .String:
		advance(parser)
		out.kind = .String
		out.text = token.text

	case .L_Brace:
		advance(parser)
		out.kind = .Object
		if parser.depth < MAX_DEPTH {
			parser.depth += 1
			parse_members(parser, out, false)
			parser.depth -= 1
		} else {
			errorf(parser, token.offset, "nesting deeper than %d levels", MAX_DEPTH)
		}

	case .L_Bracket:
		advance(parser)
		out.kind = .List
		if parser.depth < MAX_DEPTH {
			parser.depth += 1
			for {
				k := parser.token.kind
				if k == .R_Bracket {advance(parser); break}
				if k == .EOF {
					errorf(parser, token.offset, "unterminated list")
					break
				}
				if k == .Equals || k == .R_Brace {
					errorf(parser, parser.token.offset, "unexpected token in list")
					advance(parser)
					continue
				}
				element := new(Value, parser.allocator)
				parse_value(parser, element)
				if out.first == nil {
					out.first = element
					out.last = element
				} else {
					out.last.next = element
					out.last = element
				}
				out.count += 1
			}
			parser.depth -= 1
		} else {
			errorf(parser, token.offset, "nesting deeper than %d levels", MAX_DEPTH)
		}

	case .Equals, .R_Brace, .R_Bracket, .EOF:
		// An Equals, an R_Brace, an R_Bracket and the end of the input are wrong
		// at the position of a value. Report the error, leave the value nil, and
		// do NOT read the token, because the caller knows what to do with each
		// of the four.
		errorf(parser, token.offset, "expected a value")
	}
}

@(private)
parse_members :: proc(parser: ^Parser, object: ^Value, is_root: bool) {
	open_offset := parser.token.offset
	for {
		token := parser.token
		switch token.kind {
		case .EOF:
			if !is_root {errorf(parser, open_offset, "unterminated object")}
			return

		case .R_Brace:
			if is_root {
				errorf(parser, token.offset, "stray '}'")
				advance(parser)
				continue
			}
			advance(parser)
			return

		case .Atom, .String:
			advance(parser)
			if parser.token.kind == .Equals {
				advance(parser)
				node := new(Node, parser.allocator)
				node.key = token.text
				node.src_offset = token.offset
				parse_value(parser, &node.value)
				node_push_back(object, node)
			} else {
				// Do not read the token that is wrong here. It can be the next key.
				errorf(parser, token.offset, "expected '=' after key '%s'", token.text)
			}

		case .Equals:
			errorf(parser, token.offset, "expected a key before '='")
			advance(parser)
			// Read the value that has no key, where such a value follows, so that
			// the parser stays at a correct position.
			k := parser.token.kind
			if k == .Atom || k == .String || k == .L_Brace || k == .L_Bracket {
				discard: Value
				parse_value(parser, &discard)
			}

		case .L_Brace, .L_Bracket:
			errorf(parser, token.offset, "expected 'key =' before value")
			discard: Value
			parse_value(parser, &discard) // read the whole tree below this point

		case .R_Bracket:
			errorf(parser, token.offset, "stray ']'")
			advance(parser)
		}
	}
}

////////////////////////////////
//~ fp: Parse Result

Parse_Result :: struct {
	root:   ^Value, // The object that the parser makes. Read errors.count to find a problem.
	errors: Error_List,
}

parse :: proc(source: string, allocator := context.allocator) -> Parse_Result {
	parser: Parser
	parser.allocator = allocator
	parser.source = source
	advance(&parser)

	root := new(Value, allocator)
	root.kind = .Object
	parse_members(&parser, root, true)

	return {root, parser.errors}
}

// the function for the usual case
parse_file_and_report :: proc(path: string, allocator := context.allocator) -> ^Value {
	result := &nil_value
	// The tree holds views into the text of the source, so the text and the tree
	// go on one allocator.
	source, read_err := os.read_entire_file(path, allocator)
	if read_err != nil {
		fmt.eprintfln("%s: cannot read file", path)
	} else {
		parse_result := parse(string(source), allocator)
		print_errors(path, parse_result.errors)
		result = parse_result.root
	}
	return result
}

////////////////////////////////
//~ fp: Accessors
//
// Each function that takes a pointer changes a pointer of nil into &nil_value,
// and no such function gives nil back. A read that finds nothing gives the nil
// value. A key that is absent, a kind that is different, and a read inside a
// value that is not an object are the three such cases. A later read of that
// nil value gives the nil value again:
//
//   g := tb.get_num(tb.get(cfg, "physics"), "gravity") or_else 9.81
//
// That line is correct where any part of the chain is absent.
//
// Each conversion gives (value, ok). The single-value form reads as the zero
// value where the read finds nothing (ZII), `or_else` names another fallback
// at the call site, and the ok tells presence apart from a value that equals
// the fallback.

get :: proc(object: ^Value, key: string) -> ^Value {
	object := object
	if object == nil {object = &nil_value}
	result := &nil_value
	if object.kind == .Object {
		for node := object.first_member; node != nil; node = node.next {
			if node.key == key {
				result = &node.value
				break
			}
		}
	}
	return result
}

has :: proc(object: ^Value, key: string) -> bool {
	return get(object, key) != &nil_value
}

//- fp: the conversions of a value that the caller holds, such as an element of
//  a list, and a result of get

// a String value or an Identifier value
string_from_value :: proc(value: ^Value) -> (result: string, ok: bool) #optional_ok {
	if value != nil && (value.kind == .String || value.kind == .Identifier) {
		return value.text, true
	}
	return "", false
}

num_from_value :: proc(value: ^Value) -> (result: f32, ok: bool) #optional_ok {
	if value != nil && value.kind == .Number {
		return value.number, true
	}
	return 0, false
}

// yes reads as true, and no reads as false. Each other value reads as not ok,
// so `or_else` names the default at the call site.
bool_from_value :: proc(value: ^Value) -> (result: bool, ok: bool) #optional_ok {
	text := string_from_value(value) or_else ""
	switch text {
	case "yes": return true, true
	case "no":  return false, true
	}
	return false, false
}

//- fp: the reads by key

get_string :: proc(object: ^Value, key: string) -> (string, bool) #optional_ok {
	return string_from_value(get(object, key))
}

get_num :: proc(object: ^Value, key: string) -> (f32, bool) #optional_ok {
	return num_from_value(get(object, key))
}

get_bool :: proc(object: ^Value, key: string) -> (bool, bool) #optional_ok {
	return bool_from_value(get(object, key))
}

// A list of [x y], which is how a size and a position read. A key that is
// absent, a kind that is different, and a list of less than two elements each
// read as not ok.
get_v2 :: proc(object: ^Value, key: string) -> (result: geo.V2, ok: bool) #optional_ok {
	list := get(object, key)
	if list.kind != .List || list.count < 2 {return {}, false}
	result.x = num_from_value(list.first)
	result.y = num_from_value(list.first.next)
	return result, true
}

// A list of [r g b a], which is how a color reads. A list of three elements
// gives an alpha of 1. A key that is absent, a kind that is different, and a
// list of less than three elements each read as not ok.
get_v4 :: proc(object: ^Value, key: string) -> (result: geo.V4, ok: bool) #optional_ok {
	list := get(object, key)
	if list.kind != .List || list.count < 3 {return {}, false}
	result = {0, 0, 0, 1}
	i := 0
	for el := list.first; el != nil && i < 4; el = el.next {
		result[i] = num_from_value(el)
		i += 1
	}
	return result, true
}

////////////////////////////////
//~ fp: Iteration
//
// The iterator walks the members of an object, in the order of the source:
//
//   it := tabula.iterator(object)
//   for key, value in tabula.iterate(&it) { ... }
//
// The value is the value inside the node, so a write through it changes the
// tree. A pointer of nil, a value that is not an object, and the nil value
// each give an iterator with no members.

Member_Iterator :: struct {
	node: ^Node,
}

iterator :: proc(object: ^Value) -> Member_Iterator {
	if object == nil || object.kind != .Object {return {}}
	return {object.first_member}
}

iterate :: proc(it: ^Member_Iterator) -> (key: string, value: ^Value, ok: bool) {
	if it.node == nil {return}
	key = it.node.key
	value = &it.node.value
	ok = true
	it.node = it.node.next
	return
}

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
// that string for the life of the tree, so push the string on the allocator of
// the tree. A number takes a text in the format "%g", so a read of that number
// as a string is correct.

// an Object value that stands alone
build_object :: proc(allocator := context.allocator) -> ^Value {
	value := new(Value, allocator)
	value.kind = .Object
	return value
}

// a List value that stands alone
build_list :: proc(allocator := context.allocator) -> ^Value {
	value := new(Value, allocator)
	value.kind = .List
	return value
}

//- fp: The members of an object. add gives the nil value of the new member,
//  and the caller writes it. Each function below add writes that value itself.

add :: proc(object: ^Value, key: string, allocator := context.allocator) -> ^Value {
	if object == nil || object.kind != .Object {return &nil_value}
	node := new(Node, allocator)
	node.key = key
	node_push_back(object, node)
	return &node.value
}

add_string :: proc(object: ^Value, key: string, value: string, allocator := context.allocator) {
	slot := add(object, key, allocator)
	if slot != &nil_value {
		slot.kind = .String
		slot.text = value
	}
}

add_num :: proc(object: ^Value, key: string, value: f32, allocator := context.allocator) {
	slot := add(object, key, allocator)
	if slot != &nil_value {
		slot.kind = .Number
		slot.number = value
		slot.text = fmt.aprintf("%g", value, allocator = allocator)
	}
}

add_object :: proc(object: ^Value, key: string, allocator := context.allocator) -> ^Value {
	slot := add(object, key, allocator)
	if slot != &nil_value {
		slot.kind = .Object
	}
	return slot
}

add_list :: proc(object: ^Value, key: string, allocator := context.allocator) -> ^Value {
	slot := add(object, key, allocator)
	if slot != &nil_value {
		slot.kind = .List
	}
	return slot
}

//- fp: the elements of a list, with the same rules

list_push :: proc(list: ^Value, allocator := context.allocator) -> ^Value {
	if list == nil || list.kind != .List {return &nil_value}
	element := new(Value, allocator)
	if list.first == nil {
		list.first = element
		list.last = element
	} else {
		list.last.next = element
		list.last = element
	}
	list.count += 1
	return element
}

list_push_string :: proc(list: ^Value, value: string, allocator := context.allocator) {
	element := list_push(list, allocator)
	if element != &nil_value {
		element.kind = .String
		element.text = value
	}
}

list_push_num :: proc(list: ^Value, value: f32, allocator := context.allocator) {
	element := list_push(list, allocator)
	if element != &nil_value {
		element.kind = .Number
		element.number = value
		element.text = fmt.aprintf("%g", value, allocator = allocator)
	}
}

list_push_object :: proc(list: ^Value, allocator := context.allocator) -> ^Value {
	element := list_push(list, allocator)
	if element != &nil_value {
		element.kind = .Object
	}
	return element
}

////////////////////////////////
//~ fp: Error Reporting
//
// `label` names the source, such as a path or "<stdin>". An Error does not
// hold that name, because parse reads text alone. The format is
// "label:line:col: message", which is the format of gcc. An editor and a
// terminal can find a position from a line in that format.

string_from_error :: proc(label: string, error: ^Error, allocator := context.allocator) -> string {
	return fmt.aprintf(
		"%s:%d:%d: %s",
		label,
		error.line,
		error.column,
		error.message,
		allocator = allocator,
	)
}

// it writes to stderr
print_errors :: proc(label: string, errors: Error_List) {
	it := chunk.iterator(errors)
	for error in chunk.iterate(&it) {
		fmt.eprintfln("%s", string_from_error(label, error, context.temp_allocator))
	}
}

////////////////////////////////
//~ fp: Tests

// The cases below hold the decisions of the grammar: which atom is a number
// and which stays an identifier, how the parser recovers from a wrong token,
// and how a chain of reads passes the absence of a value through. The data
// files of the project parse at the end, with no error.

@(test)
numbers_and_identifiers :: proc(t: ^testing.T) {
	result := parse(
		"a = 1.50 b = 1.2.3 c = - d = -3.5 e = 2x f = \"quoted\"",
		context.temp_allocator,
	)
	testing.expect_value(t, result.errors.count, 0)

	a := get(result.root, "a")
	testing.expect_value(t, a.kind, Value_Kind.Number)
	testing.expect_value(t, a.number, 1.5)
	testing.expect_value(t, a.text, "1.50") // the text of the source stays

	testing.expect_value(t, get(result.root, "b").kind, Value_Kind.Identifier)
	testing.expect_value(t, get(result.root, "c").kind, Value_Kind.Identifier)
	testing.expect_value(t, get(result.root, "d").number, -3.5)
	testing.expect_value(t, get(result.root, "e").kind, Value_Kind.Identifier)
	testing.expect_value(t, get(result.root, "f").kind, Value_Kind.String)
	testing.expect_value(t, get(result.root, "f").text, "quoted")
}

@(test)
objects_lists_and_comments :: proc(t: ^testing.T) {
	source := `# a comment
	physics = {
		gravity = 9.81 # the comment after a value
		tags = [alpha "beta gamma" 3]
	}`
	result := parse(source, context.temp_allocator)
	testing.expect_value(t, result.errors.count, 0)

	physics := get(result.root, "physics")
	testing.expect_value(t, physics.kind, Value_Kind.Object)
	testing.expect_value(t, get_num(physics, "gravity"), 9.81)

	tags := get(physics, "tags")
	testing.expect_value(t, tags.kind, Value_Kind.List)
	testing.expect_value(t, tags.count, 3)
	testing.expect_value(t, string_from_value(tags.first), "alpha")
	testing.expect_value(t, string_from_value(tags.first.next), "beta gamma")
	testing.expect_value(t, num_from_value(tags.first.next.next), 3)
}

@(test)
nil_chain_and_fallbacks :: proc(t: ^testing.T) {
	result := parse("color = [0.1 0.2 0.3]", context.temp_allocator)

	// A chain of reads through an absent key cannot fail.
	missing := get(get(get(result.root, "no"), "such"), "path")
	testing.expect(t, value_is_nil(missing))
	testing.expect(t, !has(result.root, "no"))

	// A read that finds nothing is not ok, reads as zero, and takes an or_else.
	x, x_ok := get_num(missing, "x")
	testing.expect(t, !x_ok)
	testing.expect_value(t, x, 0)
	testing.expect_value(t, get_num(missing, "x") or_else 42, 42)

	// A list of three elements gives an alpha of 1.
	v, v_ok := get_v4(result.root, "color")
	testing.expect(t, v_ok)
	testing.expect_value(t, v, [4]f32{0.1, 0.2, 0.3, 1})
	// A key that is absent takes the whole or_else.
	testing.expect_value(t, get_v4(result.root, "no") or_else {9, 9, 9, 9}, [4]f32{9, 9, 9, 9})

	// get_v2 reads the first two elements, and follows the same rules.
	testing.expect_value(t, get_v2(result.root, "color"), geo.V2{0.1, 0.2})
	testing.expect_value(t, get_v2(result.root, "no") or_else {7, 7}, geo.V2{7, 7})

	// yes and no read as a bool. Each other value takes the whole or_else.
	flags := parse("a = yes  b = no  c = maybe", context.temp_allocator)
	testing.expect_value(t, get_bool(flags.root, "a"), true)
	testing.expect_value(t, get_bool(flags.root, "b"), false)
	testing.expect_value(t, get_bool(flags.root, "c") or_else true, true)
	testing.expect_value(t, get_bool(flags.root, "absent") or_else true, true)
}

@(test)
errors_and_positions :: proc(t: ^testing.T) {
	// The value after 'b =' is absent. The error names line 2, and the parse
	// still gives the pair of 'a'.
	result := parse("a = 1\nb = ", context.temp_allocator)
	testing.expect_value(t, result.errors.count, 1)
	testing.expect_value(t, chunk.front(result.errors).message, "expected a value")
	testing.expect_value(t, chunk.front(result.errors).line, 2)
	testing.expect_value(t, get_num(result.root, "a"), 1)

	// An unterminated string is an error, and the parse ends.
	unterminated := parse("s = \"open", context.temp_allocator)
	testing.expect_value(t, unterminated.errors.count, 1)
	testing.expect_value(t, chunk.front(unterminated.errors).message, "unterminated string")

	// A key with no '=' does not eat the next key.
	recovery := parse("lonely\nb = 2", context.temp_allocator)
	testing.expect_value(t, recovery.errors.count, 1)
	testing.expect_value(t, get_num(recovery.root, "b"), 2)
}

@(test)
member_iteration :: proc(t: ^testing.T) {
	result := parse("a = 1 b = two c = { }", context.temp_allocator)

	keys: [8]string
	count := 0
	it := iterator(result.root)
	for key, value in iterate(&it) {
		keys[count] = key
		testing.expect(t, value != nil)
		count += 1
	}
	testing.expect_value(t, count, 3)
	testing.expect_value(t, keys[0], "a")
	testing.expect_value(t, keys[1], "b")
	testing.expect_value(t, keys[2], "c")

	// A nil pointer, a value that is not an object, and an empty object each
	// give no members.
	nil_it := iterator(nil)
	for _, _ in iterate(&nil_it) {testing.fail(t)}
	number_it := iterator(get(result.root, "a"))
	for _, _ in iterate(&number_it) {testing.fail(t)}
	empty_it := iterator(get(result.root, "c"))
	for _, _ in iterate(&empty_it) {testing.fail(t)}
}

@(test)
builder :: proc(t: ^testing.T) {
	root := build_object(context.temp_allocator)
	add_num(root, "fps", 59.5, context.temp_allocator)
	add_string(root, "name", "world", context.temp_allocator)
	list := add_list(root, "items", context.temp_allocator)
	list_push_num(list, 1, context.temp_allocator)
	list_push_string(list, "two", context.temp_allocator)

	testing.expect_value(t, get_num(root, "fps"), 59.5)
	testing.expect_value(t, get(root, "fps").text, "59.5") // "%g", so the number reads as a string too
	testing.expect_value(t, get_string(root, "name"), "world")
	testing.expect_value(t, get(root, "items").count, 2)

	// An add to a nil target writes nothing, and gives the nil value.
	testing.expect(t, value_is_nil(add(&nil_value, "key", context.temp_allocator)))
}

////////////////////////////////
//~ fp: The Data Files Of The Project
//
// Each file parses with no error. The reads below hold a few values that the
// files are known to hold.

@(private)
TERRAIN_TYPES_SRC :: #load("../../data/terrain_types.tabula", string)
@(private)
UI_SRC :: #load("../../data/ui.tabula", string)
@(private)
WORLD_SRC :: #load("../../data/world.tabula", string)
@(private)
GROUP_TYPES_SRC :: #load("../../data/group_types.tabula", string)

@(test)
project_data_files :: proc(t: ^testing.T) {
	terrain := parse(TERRAIN_TYPES_SRC, context.temp_allocator)
	testing.expect_value(t, terrain.errors.count, 0)
	first_type := get(terrain.root, "terrain_type") // the first of the rows with that key
	testing.expect_value(t, get_string(first_type, "name"), "ice")
	testing.expect_value(t, get_v4(first_type, "color"), [4]f32{0.78, 0.86, 0.93, 1})

	ui := parse(UI_SRC, context.temp_allocator)
	testing.expect_value(t, ui.errors.count, 0)
	style := get(ui.root, "style")
	testing.expect_value(t, get_v4(style, "background"), [4]f32{0.91, 0.86, 0.74, 0.97})

	world := parse(WORLD_SRC, context.temp_allocator)
	testing.expect_value(t, world.errors.count, 0)
	testing.expect_value(t, get_num(get(world.root, "world"), "width"), 160)

	group := parse(GROUP_TYPES_SRC, context.temp_allocator)
	testing.expect_value(t, group.errors.count, 0)
}

