package ui

import "core:fmt"
import "core:math"
import "core:mem"
import "core:mem/virtual"
import "core:slice"
import "core:strings"
import "core:sync"
import "core:testing"

import "../chunk"
import "../geo"
import "../tabula"

////////////////////////////////
//~ fp: UI
//
// The core of an immediate mode UI. It uses geo and tb alone.
//
// The input arrives as an Input value. The result leaves as one array of
// Draw_Command. The size of a text comes from the two functions that the
// caller gives to init.
//
// The frame goes like this:
//
//   ui.frame_begin(frame_allocator, input)
//   ... build the boxes and the widgets, and read ui.signal in any order ...
//   list := ui.frame_end() // solve the layout, and make the commands
//
// The boxes make a tree. Each frame builds that tree again, on the frame
// allocator.
//
// The state that stays between two frames is in an internal table, and the key
// of a box addresses it. That state holds the animation of a hover and of a
// press, and the rects of the frame before. A read of an interaction during
// the build therefore tests against the layout of the frame before, which is
// one frame old.
//
// Each coordinate is in points, from the top left corner. Each color is a
// geo.V4 that holds RGBA from 0 to 1. Those are the rules of the draw layer,
// and this layer follows them without a dependency on it.

////////////////////////////////
//~ fp: Keys
//
// The key of a box names that box across the frames. A key is a hash of the
// string of the box, of the key of the parent box, and of the value at the top
// of the stack of seeds. Push a seed, such as the index of a loop or the id of
// a thing, to separate two boxes that a loop builds from one string.
//
// The rules for a string come from Dear ImGui and from RF:
//   "Label"          the hash of the whole string. The whole string shows.
//   "Label##suffix"  the hash of the whole string. "Label" alone shows.
//   "Label###suffix" the hash of "suffix" alone. "Label" alone shows. The
//                    text that shows can therefore change while the identity
//                    of the box stays.
//
// A string whose part for the hash is empty gives the key 0. Such a box exists
// for one frame, holds no state, and has no interaction.

Key :: distinct u64

////////////////////////////////
//~ fp: Semantic Sizes
//
// Each box gives a preferred size on each axis, and frame_end solves the tree.
//
// `strictness` is from 0 to 1. It is the part of the preferred size that the
// box keeps where the content of the parent is larger than the parent. A
// strictness of 1 keeps the whole size, and a strictness of 0 keeps none of
// it.

Axis :: enum {
	X,
	Y,
}

Size_Kind :: enum {
	Children_Sum, // The sum of the children along the axis of the children, and
	              // the largest child across that axis. It is the default (ZII).
	Points,       // the value, in points
	Text,         // The text of the box: its natural width, and a height of the
	              // number of its lines, after the wrap where the box wraps its
	              // text.
	Pct_Of_Parent, // The value, from 0 to 1, multiplied by the size of the
	              // content of the parent. It has no meaning where the parent
	              // takes its size from its children.
	Grow,         // It starts at 0. It then takes the value divided by the sum
	              // of the values of each other child that grows, of the space
	              // that the parent has left along the axis of its children.
	              // Across that axis it takes the content of the parent in full.
}

Size :: struct {
	kind: Size_Kind,
	value: f32,
	strictness: f32,
}

size_points :: proc(points, strictness: f32) -> Size {
	return {.Points, points, strictness}
}

size_pct :: proc(pct, strictness: f32) -> Size {
	return {.Pct_Of_Parent, pct, strictness}
}

size_text :: proc(strictness: f32) -> Size {
	return {.Text, 0, strictness}
}

size_children :: proc(strictness: f32) -> Size {
	return {.Children_Sum, 0, strictness}
}

size_grow :: proc(weight: f32) -> Size {
	return {.Grow, weight, 0}
}

////////////////////////////////
//~ fp: Boxes
//
// The box is the one kind of node. A widget is a set of flags and a style. The
// widget functions below make those sets, and a caller can make one directly.
//
// A box takes its style members from the stacks at the build. Those members
// stay writable until frame_end reads them.

Box_Flag :: enum {
	Clickable, // The mouse can hit it. A hover and a press then animate.
	Draw_Background,
	Draw_Border,
	Draw_Text,
	Text_Wrap, // Wrap the text to the width that the solve gives. Without this
	           // flag the text breaks at a newline character alone.
	Clip,      // clip the children and the text to the box
	Floating,  // The box is outside the layout. It stands at floating_pos,
	           // relative to its parent.
}
Box_Flags :: bit_set[Box_Flag]

Text_Align :: enum {
	Left,
	Center,
	Right,
}

// The position of the children of a box across the axis of those children. The
// children of a row therefore align on the vertical axis, and the children of
// a column align on the horizontal axis.
Align :: enum {
	Start, // at the edge of the padding (ZII)
	Center,
	End,
}

Box :: struct {
	//- The links of the tree. They exist for one frame.
	parent: ^Box,
	first: ^Box,
	last: ^Box,
	next: ^Box,
	prev: ^Box,

	//- identity
	key: Key, // A key of 0 says that the box exists for one frame.
	flags: Box_Flags,
	text: string, // the part that shows, on the frame allocator
	tags_key: u64, // It names the set of tags at the build. A color resolves
	               // against that set, and does so after the stack of tags
	               // returns to its earlier state.

	//- The style, which the box takes from the stacks at the build.
	pref_size: [Axis]Size,
	child_axis: Axis,   // the children go along this axis
	child_align: Align, // the children align in this way across that axis
	font: u64,
	font_size: f32,
	text_color: geo.V4,
	text_align: Text_Align,
	background_color: geo.V4,
	border_color: geo.V4,
	border_thickness: f32,
	corner_radius: f32,
	padding: geo.V2,         // the space at each side between the box and its content
	child_gap: f32,          // the points between two children that follow each other
	floating_pos: geo.V2,    // A floating box: the offset after the anchor below.
	floating_anchor: geo.V2, // A floating box: a fraction from 0 to 1 on each axis. It
	                         // names one point on the parent and the same point on the
	                         // box. The box stands where those two points meet, and
	                         // floating_pos then moves it. {0,0} puts the top left
	                         // corner of the box on the top left corner of the parent,
	                         // which is the default (ZII). {1,1} puts the bottom right
	                         // corner on the bottom right corner. {0.5,0.5} puts the
	                         // box at the center of the parent.

	//- frame_end writes these members. Do not read them before that call.
	fixed_size: [Axis]f32,
	rect: geo.Rect,
	lines: []string, // the lines that the frame draws, for a box with Draw_Text

	state: ^Box_State, // It is nil for a box that exists for one frame.
}

////////////////////////////////
//~ fp: Input
//
// The caller fills this struct in each frame, from the input system that it
// owns.
//
// The position of the mouse is in points. `pressed` and `released` are the two
// edges inside the frame, and `down` is the state that holds. `dt` is the
// seconds of the frame, which the animation reads. `window` is the size of the
// root box, in points.

Mouse_Button :: enum {
	Left,
	Right,
	Middle,
}

Input :: struct {
	mouse: geo.V2,
	down: [Mouse_Button]bool,
	pressed: [Mouse_Button]bool,
	released: [Mouse_Button]bool,
	scroll: geo.V2,
	dt: f32,
	window: geo.V2,
}

////////////////////////////////
//~ fp: Text Callbacks
//
// A font is a u64 handle. The two functions below read that handle, and this
// module does not.
//
// The measure function gives the size of the box around a run of text of one
// line. This module holds a cache of those sizes, whose key is the font, the
// size and the string, so it calls the function where the cache has no entry
// alone.

Font_Metrics :: struct {
	ascent: f32,       // from the top of the box to the baseline
	descent: f32,      // from the baseline to the bottom of the box
	line_advance: f32, // from one baseline to the next baseline
}

Measure_Text_Func :: proc(user: rawptr, font: u64, size: f32, text: string) -> geo.V2
Font_Metrics_Func :: proc(user: rawptr, font: u64, size: f32) -> Font_Metrics

////////////////////////////////
//~ fp: Tags / Theme
//
// A color resolves in the way that a selector of CSS does. The theme holds a
// color and nothing more: each size, each padding and each radius is a value in
// the code.
//
// A call site pushes a tag, which is a string that names what the code builds
// there, such as "button" or "accent". A color that a box does not take from a
// stack resolves against the theme.
//
// The theme is a list of patterns. Each pattern holds a list of tags and a
// color. A pattern applies where each of its tags is in the set of the tags of
// the box together with the name of the color, and where that name is among
// its tags. The names of the colors are "background", "text" and "border",
// with "hover" and "active" for a box that a person can click. The pattern
// with the most tags wins. Where no pattern applies the color is 0, which
// gives an appearance that a person sees at once.
//
// The theme is data. The caller installs it in full, and this module holds a
// reference to it. The caller must therefore keep the memory of the patterns,
// which a load from a file at the start of the program usually gives.
//
// This module holds a cache of the results, whose key is the hash of the set of
// tags. It therefore reads the patterns one time for each pair of a set of
// tags and a name, and not one time for each box.

Theme_Pattern :: struct {
	tags: []string,
	color: geo.V4,
}

Theme :: struct {
	patterns: []Theme_Pattern,
}

////////////////////////////////
//~ fp: Draw Commands
//
// The result of frame_end is one array on the frame allocator, in the order to
// draw.
//
// A Rect command holds the same values as a rect of the draw layer. A Text
// command holds one line, which contains no newline character, and its `pos`
// is the top left corner of the box around that line. A second Clip_Push
// inside the first gives the intersection of the two, and each Clip_Push has
// one Clip_Pop.

Draw_Command_Kind :: enum {
	None,
	Rect,
	Text,
	Clip_Push,
	Clip_Pop,
}

Draw_Command :: struct {
	kind: Draw_Command_Kind,

	//- Rect / Clip_Push
	rect: geo.Rect,
	colors: [geo.Corner]geo.V4,
	corner_radii: [geo.Corner]f32,
	border_thickness: f32, // A value of 0 fills the rect.
	edge_softness: f32,

	//- Text
	font: u64,
	font_size: f32,
	pos: geo.V2,
	color: geo.V4,
	text: string,
}

@(private) CMD_CHUNK_CAP :: 64
Command_List :: chunk.List(Draw_Command, CMD_CHUNK_CAP)

Draw_List :: struct {
	commands: Command_List,
	mouse_over_ui: bool, // The mouse is inside a box that a person can click, or
	                     // inside a box that draws a background. It is also true
	                     // while a press that started on such a box continues.
}

////////////////////////////////
//~ fp: UI State
//
// This module holds one global, as each gfx module does.
//
// The frame allocator and each thing on it, which is a box, a line and a draw
// command, exist for one frame. The arena of the module holds the table of the
// box states and the sets of tags, for the life of the module.
//
// A box state that no box read in the last frame goes to a free list at the
// next frame_begin, and a later box can take it.

@(private) STACK_CAP :: 64
@(private) STATE_BUCKETS :: 256    // a power of two
@(private) TEXT_CACHE_SLOTS :: 4096 // a power of two
@(private) TAG_SET_BUCKETS :: 64   // a power of two
@(private) COLOR_CACHE_SLOTS :: 1024 // a power of two

// the state that stays between two frames, which this module alone reads
Box_State :: struct {
	hash_next: ^Box_State, // the next state of this bucket, or the next state of the free list
	key: Key,
	last_frame_touched: u64,
	rect: geo.Rect, // the rect of the last frame in which a caller built this box
	rect_valid: bool,
	hot_t: f32,    // From 0 to 1. It moves toward 1 while this box is the hot box.
	active_t: f32, // From 0 to 1. It moves toward 1 while this box is the active box.
}

// The cache of the text sizes. Each key maps to one slot, and a second key
// that maps to that slot writes over the first.
@(private)
Text_Cache_Slot :: struct {
	key: u64, // A key of 0 says that the slot is empty.
	dim: geo.V2,
}

// The key of a set of tags, and the strings of the tags that the key stands
// for. push_tag writes this node on the arena of the module, so that a later
// read of a color can find the strings of the tags_key of a box, and can test
// them against the patterns.
@(private)
Tag_Set_Node :: struct {
	next: ^Tag_Set_Node,
	key: u64,
	tags: []string,
}

// The cache of the colors. Its key is a set of tags and the name of a color.
// Each key maps to one slot, and a second key that maps to that slot writes
// over the first. set_theme clears the whole cache.
@(private)
Color_Cache_Slot :: struct {
	key: u64, // 0 = empty
	color: geo.V4,
}

// One stack of the style. The entry at the bottom is the default of the frame,
// which frame_begin puts there, so a pop cannot remove it.
@(private)
Stack :: struct($T: typeid) {
	items: [STACK_CAP]T,
	top: int,
}

@(private)
stack_push :: proc(s: ^Stack($T), v: T) {
	assert(s.top < STACK_CAP)
	s.items[s.top] = v
	s.top += 1
}

@(private)
stack_pop :: proc(s: ^Stack($T)) {
	assert(s.top > 1)
	s.top -= 1
}

@(private)
stack_top :: proc(s: ^Stack($T)) -> T {
	return s.items[s.top - 1]
}

@(private)
stack_pushed :: proc(s: ^Stack($T)) -> bool {
	return s.top > 1 // it holds more than its default entry
}

@(private)
stack_reset :: proc(s: ^Stack($T), default_value: T) {
	s.top = 0
	stack_push(s, default_value)
}

@(private)
State :: struct {
	//- the members that init writes
	initialized: bool,
	arena: virtual.Arena,
	persist: mem.Allocator,
	measure: Measure_Text_Func,
	metrics: Font_Metrics_Func,
	user: rawptr,
	theme: Theme, // The caller owns the memory of the patterns. set_theme installs it.
	text_cache: [TEXT_CACHE_SLOTS]Text_Cache_Slot,
	color_cache: [COLOR_CACHE_SLOTS]Color_Cache_Slot,
	tag_set_buckets: [TAG_SET_BUCKETS]^Tag_Set_Node,
	state_buckets: [STATE_BUCKETS]^Box_State,
	state_free: ^Box_State,

	//- the members of one frame
	frame_allocator: mem.Allocator, // The frame allocator of the caller. This module holds it between the begin and the end.
	input: Input,
	frame: u64,
	root: ^Box,
	cmds: Command_List,

	//- the members of the interaction
	hot_key: Key,      // The box that a person can click, that is under the
	                   // mouse, and that is above each other such box, as of the
	                   // frame before. It stays equal to active_key while the
	                   // button is down.
	hot_key_next: Key, // The emission computes it, for the next frame.
	active_key: Key,   // the box on which the left button went down, until that button comes up
	press_mouse: geo.V2, // the position of the mouse at that press
	mouse_over: bool,  // the emission computes it

	//- The stacks of the style.
	parent_stack: Stack(^Box),
	tag_stack: Stack(string),
	tag_key_stack: Stack(u64), // the hash of the contents of tag_stack, which grows with each entry
	seed_stack: Stack(u64),
	pref_width_stack: Stack(Size),
	pref_height_stack: Stack(Size),
	font_stack: Stack(u64),
	font_size_stack: Stack(f32),
	text_color_stack: Stack(geo.V4),
	text_align_stack: Stack(Text_Align),
	background_color_stack: Stack(geo.V4),
	border_color_stack: Stack(geo.V4),
	border_thickness_stack: Stack(f32),
	corner_radius_stack: Stack(f32),
	child_axis_stack: Stack(Axis),
	child_align_stack: Stack(Align),
	padding_stack: Stack(geo.V2),
	child_gap_stack: Stack(f32),
}

@(private) state: State

////////////////////////////////
//~ fp: Small Helpers

// The result holds min, and does not hold max.
@(private)
rect_contains :: proc(r: geo.Rect, p: geo.V2) -> bool {
	return p.x >= r.min.x && p.x < r.max.x && p.y >= r.min.y && p.y < r.max.y
}

//- fp: fnv-1a across the bytes, then the final step of splitmix64. Two inputs
//  that follow each other therefore give two hashes that are far apart.
@(private)
hash_string :: proc(seed: u64, s: string) -> u64 {
	h := seed ~ 0xcbf29ce484222325
	for i in 0 ..< len(s) {
		h = (h ~ u64(s[i])) * 0x100000001b3
	}
	return h
}

@(private)
mix64 :: proc(h: u64) -> u64 {
	h := h
	h ~= h >> 30
	h *= 0xbf58476d1ce4e5b9
	h ~= h >> 27
	h *= 0x94d049bb133111eb
	h ~= h >> 31
	return h
}

//- fp: the split at "##" and at "###". See the Keys section above.
@(private)
display_part :: proc(s: string) -> string {
	idx := strings.index(s, "##")
	return (idx < 0) ? s : s[:idx]
}

@(private)
hash_part :: proc(s: string) -> string {
	idx := strings.index(s, "###")
	return (idx < 0) ? s : s[idx + 3:]
}

////////////////////////////////
//~ fp: Text Measurement
//
// Each measurement goes through the cache. The function of the caller runs
// where the cache has no entry alone. The key is the font, the bits of the
// size, and the bytes of the string.

@(private)
measure_text :: proc(font: u64, size: f32, text: string) -> geo.V2 {
	s := &state
	if s.measure == nil {
		return {}
	}
	size_bits := transmute(u32)size
	key := mix64(hash_string(font ~ (u64(size_bits) << 32), text))
	if key == 0 { key = 1 }
	slot := &s.text_cache[key & (TEXT_CACHE_SLOTS - 1)]
	if slot.key != key {
		slot.key = key
		slot.dim = s.measure(s.user, font, size, text)
	}
	return slot.dim
}

@(private)
font_metrics :: proc(font: u64, size: f32) -> Font_Metrics {
	s := &state
	if s.metrics == nil {
		return {}
	}
	return s.metrics(s.user, font, size)
}

////////////////////////////////
//~ fp: Persistent Box State

@(private)
state_for_key :: proc(key: Key) -> ^Box_State {
	s := &state
	bucket := &s.state_buckets[u64(key) & (STATE_BUCKETS - 1)]
	bs := bucket^
	for bs != nil && bs.key != key { bs = bs.hash_next }
	if bs == nil {
		if s.state_free != nil {
			bs = s.state_free
			s.state_free = bs.hash_next
		} else {
			bs = new(Box_State, s.persist)
		}
		bs^ = {}
		bs.key = key
		bs.hash_next = bucket^
		bucket^ = bs
	}
	bs.last_frame_touched = s.frame
	return bs
}

////////////////////////////////
//~ fp: Style Stacks
//
// frame_begin sets each stack to a default that suits a struct of zeros. A box
// takes the value at the top of each stack at its build. Each push needs a pop
// inside the same frame, which an assert tests.

push_parent :: proc(box: ^Box) { stack_push(&state.parent_stack, box) }
pop_parent :: proc() { stack_pop(&state.parent_stack) }

push_seed :: proc(v: u64) { stack_push(&state.seed_stack, v) }
pop_seed :: proc() { stack_pop(&state.seed_stack) }
push_pref_width :: proc(v: Size) { stack_push(&state.pref_width_stack, v) }
pop_pref_width :: proc() { stack_pop(&state.pref_width_stack) }
push_pref_height :: proc(v: Size) { stack_push(&state.pref_height_stack, v) }
pop_pref_height :: proc() { stack_pop(&state.pref_height_stack) }
push_font :: proc(v: u64) { stack_push(&state.font_stack, v) }
pop_font :: proc() { stack_pop(&state.font_stack) }
push_font_size :: proc(v: f32) { stack_push(&state.font_size_stack, v) }
pop_font_size :: proc() { stack_pop(&state.font_size_stack) }
push_text_color :: proc(v: geo.V4) { stack_push(&state.text_color_stack, v) }
pop_text_color :: proc() { stack_pop(&state.text_color_stack) }
push_text_align :: proc(v: Text_Align) { stack_push(&state.text_align_stack, v) }
pop_text_align :: proc() { stack_pop(&state.text_align_stack) }
push_background_color :: proc(v: geo.V4) { stack_push(&state.background_color_stack, v) }
pop_background_color :: proc() { stack_pop(&state.background_color_stack) }
push_border_color :: proc(v: geo.V4) { stack_push(&state.border_color_stack, v) }
pop_border_color :: proc() { stack_pop(&state.border_color_stack) }
push_border_thickness :: proc(v: f32) { stack_push(&state.border_thickness_stack, v) }
pop_border_thickness :: proc() { stack_pop(&state.border_thickness_stack) }
push_corner_radius :: proc(v: f32) { stack_push(&state.corner_radius_stack, v) }
pop_corner_radius :: proc() { stack_pop(&state.corner_radius_stack) }
push_child_axis :: proc(v: Axis) { stack_push(&state.child_axis_stack, v) }
pop_child_axis :: proc() { stack_pop(&state.child_axis_stack) }
push_child_align :: proc(v: Align) { stack_push(&state.child_align_stack, v) }
pop_child_align :: proc() { stack_pop(&state.child_align_stack) }
push_padding :: proc(v: geo.V2) { stack_push(&state.padding_stack, v) }
pop_padding :: proc() { stack_pop(&state.padding_stack) }
push_child_gap :: proc(v: f32) { stack_push(&state.child_gap_stack, v) }
pop_child_gap :: proc() { stack_pop(&state.child_gap_stack) }

////////////////////////////////
//~ fp: Tags / Theme

set_theme :: proc(theme: Theme) {
	state.theme = theme
	state.color_cache = {}
}

// This function reads data/ui.tabula into a Theme. The objects inside each
// other are the selector: the path of the keys down to a color becomes the
// list of tags of a pattern. style.button.background therefore gives the tags
// ["button" "background"].
//
// This function knows no key name. The vocabulary is in the file, and at each
// call site that pushes a tag.
//
// A leaf must be a color of [r g b] or [r g b a], which an assert tests,
// because the theme holds no size. A pattern that is absent gives a color of 0
// in this module, which shows at once. This module never puts a default color
// there without a report.
@(private)
theme_walk :: proc(object: ^tabula.Value, path: ^[8]string, depth: int,
                   patterns: ^[dynamic]Theme_Pattern, allocator: mem.Allocator) {
	for node := object.first_member; node != nil; node = node.next {
		assert(depth < len(path)) // the number of entries of path
		path[depth] = node.key
		if node.value.kind == .Object {
			theme_walk(&node.value, path, depth + 1, patterns, allocator)
		} else {
			list := &node.value
			assert(list.kind == .List && list.count >= 3)
			color := geo.V4{0, 0, 0, 1}
			ci := 0
			for el := list.first; el != nil && ci < 4; el = el.next {
				color[ci] = tabula.num_from_value(el)
				ci += 1
			}
			pattern: Theme_Pattern
			pattern.tags = make([]string, depth + 1, allocator)
			for i in 0 ..= depth {
				pattern.tags[i] = strings.clone(path[i], allocator)
			}
			pattern.color = color
			append(patterns, pattern)
		}
	}
}

theme_load :: proc(path: string, allocator := context.allocator) -> Theme {
	theme: Theme
	root := tabula.parse_file_and_report(path, context.temp_allocator)
	style := tabula.get(root, "style")
	tag_path: [8]string
	patterns := make([dynamic]Theme_Pattern, context.temp_allocator)
	theme_walk(style, &tag_path, 0, &patterns, allocator)
	theme.patterns = slice.clone(patterns[:], allocator)
	// The C build fills its array from a prepended list, so a tie between two
	// patterns of one length resolves to the later one in the file. Keep that
	// order.
	slice.reverse(theme.patterns)
	return theme
}

// A push adds to the hash of the stack. The first push that makes a given set
// of tags copies the strings of that set to the arena of the module. A key
// therefore gives its strings back for the life of the module.
push_tag :: proc(t: string) {
	s := &state
	key := mix64(hash_string(stack_top(&s.tag_key_stack), t))
	if key == 0 { key = 1 }
	stack_push(&s.tag_stack, t)
	stack_push(&s.tag_key_stack, key)
	bucket := key & (TAG_SET_BUCKETS - 1)
	node := s.tag_set_buckets[bucket]
	for node != nil && node.key != key { node = node.next }
	if node == nil {
		node = new(Tag_Set_Node, s.persist)
		node.key = key
		count := s.tag_stack.top - 1 // the entries above the empty entry at the bottom
		node.tags = make([]string, count, s.persist)
		for i in 0 ..< count {
			node.tags[i] = strings.clone(s.tag_stack.items[i + 1], s.persist)
		}
		node.next = s.tag_set_buckets[bucket]
		s.tag_set_buckets[bucket] = node
	}
}

pop_tag :: proc() {
	stack_pop(&state.tag_stack)
	stack_pop(&state.tag_key_stack)
}

// The pattern that matches and that holds the most tags wins. A pattern
// matches where each of its tags is in the set of tags together with the name,
// and where the name itself is among its tags.
@(private)
color_from_key_name :: proc(tags_key: u64, name: string) -> geo.V4 {
	s := &state
	final_key := mix64(hash_string(tags_key, name))
	if final_key == 0 { final_key = 1 }
	slot := &s.color_cache[final_key & (COLOR_CACHE_SLOTS - 1)]
	if slot.key != final_key {
		tags: []string
		for n := s.tag_set_buckets[tags_key & (TAG_SET_BUCKETS - 1)]; n != nil; n = n.next {
			if n.key == tags_key {
				tags = n.tags
				break
			}
		}
		color: geo.V4
		best := 0
		for &p in s.theme.patterns {
			has_name := false
			all_present := true
			for t := 0; t < len(p.tags) && all_present; t += 1 {
				present := p.tags[t] == name
				has_name |= present
				for k := 0; !present && k < len(tags); k += 1 {
					present = p.tags[t] == tags[k]
				}
				all_present = present
			}
			if has_name && all_present && len(p.tags) > best {
				best = len(p.tags)
				color = p.color
			}
		}
		slot.key = final_key
		slot.color = color
	}
	return slot.color
}

// resolve the name against the tags that are on the stack now
color_from_name :: proc(name: string) -> geo.V4 {
	return color_from_key_name(stack_top(&state.tag_key_stack), name)
}

////////////////////////////////
//~ fp: Frame Lifecycle

init :: proc(measure: Measure_Text_Func, metrics: Font_Metrics_Func, user: rawptr) {
	state = {}
	if virtual.arena_init_growing(&state.arena) != nil { panic("out of memory: ui arena") }
	state.persist = virtual.arena_allocator(&state.arena)
	state.measure = measure
	state.metrics = metrics
	state.user = user
	state.initialized = true
}

// the root box of this frame, at the size of the window
root :: proc() -> ^Box {
	return state.root
}

// the mouse of this frame, in points
mouse :: proc() -> geo.V2 {
	return state.input.mouse
}

frame_begin :: proc(frame_allocator: mem.Allocator, input: Input) {
	s := &state
	assert(s.initialized) // a caller must call init first
	s.frame_allocator = frame_allocator
	s.input = input
	s.frame += 1
	s.mouse_over = false
	s.cmds = {}

	//- The hover follows the box that the frame before found under the mouse
	//  and above each other such box. A button that stays down holds it.
	s.hot_key = (s.active_key != 0) ? s.active_key : s.hot_key_next
	s.hot_key_next = 0

	//- Take back each state that no box read in the frame before.
	for b in 0 ..< STATE_BUCKETS {
		for at := &s.state_buckets[b]; at^ != nil; {
			bs := at^
			if bs.last_frame_touched + 1 < s.frame {
				at^ = bs.hash_next
				bs.hash_next = s.state_free
				s.state_free = bs
			} else {
				at = &bs.hash_next
			}
		}
	}

	//- The root box, at the size of the window. Its key is 0, so it exists for
	//  one frame, and it takes no value from a stack.
	root_box := new(Box, s.frame_allocator)
	root_box.pref_size[.X] = size_points(input.window.x, 1)
	root_box.pref_size[.Y] = size_points(input.window.y, 1)
	root_box.child_axis = .Y
	s.root = root_box

	//- Set each stack to its default. No code reads the entry at the bottom of
	//  a stack of colors: a color whose stack holds no push comes from the
	//  theme.
	stack_reset(&s.parent_stack, root_box)
	stack_reset(&s.tag_stack, "")
	stack_reset(&s.tag_key_stack, 0)
	stack_reset(&s.seed_stack, 0)
	stack_reset(&s.pref_width_stack, size_children(0))
	stack_reset(&s.pref_height_stack, size_children(0))
	stack_reset(&s.font_stack, u64(0))
	stack_reset(&s.font_size_stack, f32(16.0))
	stack_reset(&s.text_color_stack, geo.V4{})
	stack_reset(&s.text_align_stack, Text_Align.Left)
	stack_reset(&s.background_color_stack, geo.V4{})
	stack_reset(&s.border_color_stack, geo.V4{})
	stack_reset(&s.border_thickness_stack, f32(1.0))
	stack_reset(&s.corner_radius_stack, f32(0.0))
	stack_reset(&s.child_axis_stack, Axis.Y)
	stack_reset(&s.child_align_stack, Align.Start)
	stack_reset(&s.padding_stack, geo.V2{})
	stack_reset(&s.child_gap_stack, f32(0.0))
}

////////////////////////////////
//~ fp: Building / Signals
//
// box adds a box under the current parent, and gives that box back, so that
// the caller can write its members.
//
// signal reads the interaction of a box against the layout of the frame
// before. `hovered` follows the box that a person can click and that is above
// each other such box under the mouse. `clicked` says that the left button
// came up over the box on which it went down.

box :: proc(flags: Box_Flags, str: string) -> ^Box {
	s := &state
	parent_box := stack_top(&s.parent_stack)
	b := new(Box, s.frame_allocator)

	hashed := hash_part(str)
	if len(hashed) > 0 {
		seed := u64(parent_box.key) ~ stack_top(&s.seed_stack)
		b.key = Key(mix64(hash_string(seed, hashed)))
		if b.key == 0 { b.key = 1 }
		b.state = state_for_key(b.key)
	}
	b.flags = flags
	b.text = strings.clone(display_part(str), s.frame_allocator)

	b.pref_size[.X] = stack_top(&s.pref_width_stack)
	b.pref_size[.Y] = stack_top(&s.pref_height_stack)
	b.child_axis = stack_top(&s.child_axis_stack)
	b.child_align = stack_top(&s.child_align_stack)
	b.font = stack_top(&s.font_stack)
	b.text_align = stack_top(&s.text_align_stack)
	b.font_size = stack_top(&s.font_size_stack)
	b.border_thickness = stack_top(&s.border_thickness_stack)
	b.corner_radius = stack_top(&s.corner_radius_stack)
	b.padding = stack_top(&s.padding_stack)
	b.child_gap = stack_top(&s.child_gap_stack)

	//- The colors. A stack with a push wins against the theme.
	b.tags_key = stack_top(&s.tag_key_stack)
	b.text_color = stack_pushed(&s.text_color_stack) ? stack_top(&s.text_color_stack) : color_from_key_name(b.tags_key, "text")
	b.background_color = stack_pushed(&s.background_color_stack) ? stack_top(&s.background_color_stack) : color_from_key_name(b.tags_key, "background")
	b.border_color = stack_pushed(&s.border_color_stack) ? stack_top(&s.border_color_stack) : color_from_key_name(b.tags_key, "border")

	b.parent = parent_box
	if parent_box.first == nil {
		parent_box.first = b
		parent_box.last = b
	} else {
		b.prev = parent_box.last
		parent_box.last.next = b
		parent_box.last = b
	}
	return b
}

boxf :: proc(flags: Box_Flags, format: string, args: ..any) -> ^Box {
	return box(flags, fmt.aprintf(format, ..args, allocator = state.frame_allocator))
}

signal :: proc(b: ^Box) -> Signal {
	s := &state
	sig: Signal
	sig.box = b
	if b == nil || b.key == 0 {
		return sig
	}
	in_ := &s.input
	hot := s.hot_key == b.key
	sig.hovered = hot
	if .Clickable in b.flags && hot && in_.pressed[.Left] {
		s.active_key = b.key
		s.press_mouse = in_.mouse
		sig.pressed = true
	}
	if s.active_key == b.key {
		sig.down = in_.down[.Left]
		sig.released = in_.released[.Left]
		sig.clicked = sig.released && b.state.rect_valid &&
		              rect_contains(b.state.rect, in_.mouse)
		sig.drag_delta = in_.mouse - s.press_mouse
	}
	if hot && in_.pressed[.Right] {
		sig.right_clicked = true
	}
	return sig
}

Signal :: struct {
	box: ^Box, // the box of this signal, so that the caller can write its members
	hovered: bool,
	pressed: bool,  // the left button went down on the box in this frame
	down: bool,     // the left button stays down since that press
	released: bool, // the left button came up in this frame, at any position of the mouse
	clicked: bool,
	right_clicked: bool,
	drag_delta: geo.V2, // the mouse minus the position of the press, while the button is down
}

////////////////////////////////
//~ fp: Layout
//
// The solve goes one axis at a time. Each pass runs for X. The text then
// breaks into lines against those widths. Each pass then runs for Y, so a
// height that comes from a wrap reads a width that the solve gave. One more
// pass then puts each box at its position on both axes.
//
// Inside one axis the passes go in this order: the kinds that need no other
// box; then the percent of the parent, from the root down; then the sum of the
// children, from the leaves up; then the growth, from the root down; then the
// correction of each box that is too large, from the root down.
//
// The growth runs before that correction. A parent has space that it has left,
// or content that is larger than it, and never both, so the two passes act on
// two sets of parents that do not meet.
//
// A floating box takes no part in a sum, in the growth, in the correction, and
// in the position of the boxes of the layout.

@(private)
layout_standalone :: proc(b: ^Box, axis: Axis) {
	size := b.pref_size[axis]
	pad2 := 2 * b.padding[int(axis)]
	switch size.kind {
	case .Points:
		b.fixed_size[axis] = size.value
	case .Grow:
		b.fixed_size[axis] = 0
	case .Text:
		if axis == .X {
			// the natural width, which is the width of the widest line between
			// two newline characters
			w: f32 = 0
			s := b.text
			start := 0
			for i in 0 ..= len(s) {
				if i == len(s) || s[i] == '\n' {
					w = max(w, measure_text(b.font, b.font_size, s[start:i]).x)
					start = i + 1
				}
			}
			b.fixed_size[axis] = w + pad2
		} else {
			m := font_metrics(b.font, b.font_size)
			h: f32 = 0
			if len(b.lines) > 0 {
				h = f32(len(b.lines) - 1) * m.line_advance + m.ascent + m.descent
			}
			b.fixed_size[axis] = h + pad2
		}
	case .Children_Sum, .Pct_Of_Parent:
		// A later pass gives a size to these two.
	}
	for child := b.first; child != nil; child = child.next {
		layout_standalone(child, axis)
	}
}

@(private)
layout_pct :: proc(b: ^Box, axis: Axis) {
	if b.pref_size[axis].kind == .Pct_Of_Parent && b.parent != nil {
		content := b.parent.fixed_size[axis] - 2 * b.parent.padding[int(axis)]
		b.fixed_size[axis] = max(content, 0) * b.pref_size[axis].value
	}
	for child := b.first; child != nil; child = child.next {
		layout_pct(child, axis)
	}
}

@(private)
layout_sum :: proc(b: ^Box, axis: Axis) {
	for child := b.first; child != nil; child = child.next {
		layout_sum(child, axis)
	}
	if b.pref_size[axis].kind == .Children_Sum {
		total: f32 = 0
		n := 0
		for child := b.first; child != nil; child = child.next {
			if .Floating in child.flags { continue }
			if axis == b.child_axis {
				total += child.fixed_size[axis]
			} else {
				total = max(total, child.fixed_size[axis])
			}
			n += 1
		}
		if axis == b.child_axis && n > 1 {
			total += b.child_gap * f32(n - 1)
		}
		b.fixed_size[axis] = total + 2 * b.padding[int(axis)]
	}
}

@(private)
layout_grow :: proc(b: ^Box, axis: Axis) {
	content := max(b.fixed_size[axis] - 2 * b.padding[int(axis)], 0)
	if axis == b.child_axis {
		//- Divide the space that the parent has left between the children that
		//  grow, in the proportion of their weights.
		total: f32 = 0
		weights: f32 = 0
		n := 0
		for child := b.first; child != nil; child = child.next {
			if .Floating in child.flags { continue }
			total += child.fixed_size[axis]
			if child.pref_size[axis].kind == .Grow {
				weights += child.pref_size[axis].value
			}
			n += 1
		}
		if n > 1 {
			total += b.child_gap * f32(n - 1)
		}
		leftover := content - total
		if leftover > 0 && weights > 0 {
			for child := b.first; child != nil; child = child.next {
				if .Floating in child.flags { continue }
				if child.pref_size[axis].kind == .Grow {
					child.fixed_size[axis] += leftover * child.pref_size[axis].value / weights
				}
			}
		}
	} else {
		//- Across the axis of the children, a child that grows takes the content
		//  of the parent in full.
		for child := b.first; child != nil; child = child.next {
			if .Floating in child.flags { continue }
			if child.pref_size[axis].kind == .Grow {
				child.fixed_size[axis] = content
			}
		}
	}
	for child := b.first; child != nil; child = child.next {
		layout_grow(child, axis)
	}
}

@(private)
layout_violations :: proc(b: ^Box, axis: Axis) {
	content := b.fixed_size[axis] - 2 * b.padding[int(axis)]
	if axis == b.child_axis {
		//- The children are larger than the parent along the axis of the layout.
		//  Take back the difference from each child, in the proportion of its
		//  size multiplied by (1 - strictness).
		total: f32 = 0
		n := 0
		for child := b.first; child != nil; child = child.next {
			if .Floating in child.flags { continue }
			total += child.fixed_size[axis]
			n += 1
		}
		if n > 1 {
			total += b.child_gap * f32(n - 1)
		}
		over := total - content
		if over > 0 {
			budget: f32 = 0
			for child := b.first; child != nil; child = child.next {
				if .Floating in child.flags { continue }
				budget += child.fixed_size[axis] * (1 - child.pref_size[axis].strictness)
			}
			if budget > 0 {
				f := min(1.0, over / budget)
				for child := b.first; child != nil; child = child.next {
					if .Floating in child.flags { continue }
					give := child.fixed_size[axis] * (1 - child.pref_size[axis].strictness) * f
					child.fixed_size[axis] = max(child.fixed_size[axis] - give, 0)
				}
			}
		}
	} else {
		//- Across that axis, each child clamps to the size of the content of the
		//  parent.
		for child := b.first; child != nil; child = child.next {
			if .Floating in child.flags { continue }
			over := child.fixed_size[axis] - content
			if over > 0 {
				give := over * (1 - child.pref_size[axis].strictness)
				child.fixed_size[axis] = max(child.fixed_size[axis] - give, 0)
			}
		}
	}
	for child := b.first; child != nil; child = child.next {
		layout_violations(child, axis)
	}
}

// Break the string of each box with Draw_Text into the lines that the frame
// draws. The string always breaks at a newline character. Where the box wraps
// its text, the string also breaks between two words, and each line takes as
// many words as the width that the solve gave holds. A word that is wider than
// that width takes a line alone, and goes past the box.
@(private)
build_lines :: proc(b: ^Box) {
	if .Draw_Text in b.flags {
		s := b.text
		cap := 1
		for i in 0 ..< len(s) {
			if s[i] == ' ' || s[i] == '\n' { cap += 1 }
		}
		buf := make([]string, cap, state.frame_allocator)
		n := 0
		avail := b.fixed_size[.X] - 2 * b.padding.x
		start := 0
		for i in 0 ..= len(s) {
			if i == len(s) || s[i] == '\n' {
				para := s[start:i]
				if .Text_Wrap not_in b.flags || len(para) == 0 {
					buf[n] = para
					n += 1
				} else {
					pos := 0
					for pos < len(para) {
						for pos < len(para) && para[pos] == ' ' { pos += 1 }
						if pos >= len(para) { break }
						line_start := pos
						line_end := pos
						for scan := pos; scan < len(para); {
							for scan < len(para) && para[scan] == ' ' { scan += 1 }
							word_start := scan
							for scan < len(para) && para[scan] != ' ' { scan += 1 }
							if word_start == scan { break }
							cand := para[line_start:scan]
							if line_end == line_start ||
							   measure_text(b.font, b.font_size, cand).x <= avail {
								line_end = scan
							} else {
								break
							}
						}
						buf[n] = para[line_start:line_end]
						n += 1
						pos = line_end
					}
				}
				start = i + 1
			}
		}
		assert(n <= cap)
		b.lines = buf[:n]
	}
	for child := b.first; child != nil; child = child.next {
		build_lines(child)
	}
}

@(private)
layout_position :: proc(b: ^Box) {
	on := b.child_axis
	off: Axis = (on == .X) ? .Y : .X
	align: f32 = (b.child_align == .Center) ? 0.5 : (b.child_align == .End) ? 1.0 : 0.0
	off_content := b.fixed_size[off] - 2 * b.padding[int(off)]
	cursor := b.rect.min + b.padding
	for child := b.first; child != nil; child = child.next {
		p := cursor
		if .Floating in child.flags {
			a := child.floating_anchor
			p = {b.rect.min.x + a.x * (b.fixed_size[.X] - child.fixed_size[.X]) + child.floating_pos.x,
			     b.rect.min.y + a.y * (b.fixed_size[.Y] - child.fixed_size[.Y]) + child.floating_pos.y}
		} else {
			// The alignment across the axis of the children. A child stands at the
			// edge of the padding where the alignment is Start alone. A child that
			// is larger than the parent stays at that edge.
			slack := max(off_content - child.fixed_size[off], 0) * align
			if off == .X {
				p.x += slack
			} else {
				p.y += slack
			}
			if on == .X {
				cursor.x += child.fixed_size[.X] + b.child_gap
			} else {
				cursor.y += child.fixed_size[.Y] + b.child_gap
			}
		}
		child.rect = {p, {p.x + child.fixed_size[.X], p.y + child.fixed_size[.Y]}}
		layout_position(child)
	}
}

////////////////////////////////
//~ fp: Command Emission
//
// One walk from the root down, in the order to draw: the background, the push
// of the clip, the text, the children, the pop of the clip, and the border.
// The border comes last, so it draws over a child that goes past the box.
//
// The same walk writes the rect and the animation of the state of each box,
// and finds the hot box of the next frame. The last box that the mouse hits in
// that order is the box above each other such box.

@(private)
push_cmd :: proc(kind: Draw_Command_Kind) -> ^Draw_Command {
	s := &state
	return chunk.push(&s.cmds, Draw_Command{kind = kind}, s.frame_allocator)
}

@(private)
emit :: proc(b: ^Box) {
	s := &state

	if b.state != nil {
		bs := b.state
		bs.rect = b.rect
		bs.rect_valid = true
		blend := 1.0 - math.pow(2.0, -24.0 * s.input.dt)
		hot_target: f32 = (s.hot_key == b.key) ? 1.0 : 0.0
		active_target: f32 = (s.active_key == b.key) ? 1.0 : 0.0
		bs.hot_t += (hot_target - bs.hot_t) * blend
		bs.active_t += (active_target - bs.active_t) * blend
	}

	if rect_contains(b.rect, s.input.mouse) {
		if .Clickable in b.flags && b.key != 0 {
			s.hot_key_next = b.key
		}
		if b.flags & {.Clickable, .Draw_Background} != {} {
			s.mouse_over = true
		}
	}

	if .Draw_Background in b.flags {
		bg := b.background_color
		if .Clickable in b.flags && b.state != nil {
			// The hover color and the active color of the theme move the
			// background toward themselves while the box is hot, and while a
			// person holds it. The alpha of each of those two colors is the
			// strength of that movement.
			//
			// This code reads those colors here, after the stack of tags returned
			// to its earlier state, because the tags_key of the box still names
			// the set of tags of its build.
			hover := color_from_key_name(b.tags_key, "hover")
			active := color_from_key_name(b.tags_key, "active")
			fh := hover.a * b.state.hot_t
			fa := active.a * b.state.active_t
			bg.r += (hover.r - bg.r) * fh + (active.r - bg.r) * fa
			bg.g += (hover.g - bg.g) * fh + (active.g - bg.g) * fa
			bg.b += (hover.b - bg.b) * fh + (active.b - bg.b) * fa
		}
		cmd := push_cmd(.Rect)
		cmd.rect = b.rect
		for c in geo.Corner {
			cmd.colors[c] = bg
			cmd.corner_radii[c] = b.corner_radius
		}
		cmd.edge_softness = (b.corner_radius > 0) ? 1.0 : 0.0
	}

	if .Clip in b.flags {
		cmd := push_cmd(.Clip_Push)
		cmd.rect = b.rect
	}

	if .Draw_Text in b.flags && len(b.lines) > 0 {
		m := font_metrics(b.font, b.font_size)
		tl := b.rect.min + b.padding
		content_w := b.fixed_size[.X] - 2 * b.padding.x
		for line, i in b.lines {
			if len(line) == 0 { continue }
			x := tl.x
			if b.text_align != .Left {
				w := measure_text(b.font, b.font_size, line).x
				x += (b.text_align == .Center) ? (content_w - w) * 0.5 : (content_w - w)
			}
			cmd := push_cmd(.Text)
			cmd.font = b.font
			cmd.font_size = b.font_size
			cmd.pos = {x, tl.y + f32(i) * m.line_advance}
			cmd.color = b.text_color
			cmd.text = line
		}
	}

	for child := b.first; child != nil; child = child.next {
		emit(child)
	}

	if .Clip in b.flags {
		push_cmd(.Clip_Pop)
	}

	if .Draw_Border in b.flags && b.border_thickness > 0 {
		cmd := push_cmd(.Rect)
		cmd.rect = b.rect
		for c in geo.Corner {
			cmd.colors[c] = b.border_color
			cmd.corner_radii[c] = b.corner_radius
		}
		cmd.border_thickness = b.border_thickness
		cmd.edge_softness = 1.0
	}
}

frame_end :: proc() -> Draw_List {
	s := &state
	assert(s.parent_stack.top == 1) // each begin needs its end
	root_box := s.root

	layout_standalone(root_box, .X)
	layout_pct(root_box, .X)
	layout_sum(root_box, .X)
	layout_grow(root_box, .X)
	layout_violations(root_box, .X)
	build_lines(root_box)
	layout_standalone(root_box, .Y)
	layout_pct(root_box, .Y)
	layout_sum(root_box, .Y)
	layout_grow(root_box, .Y)
	layout_violations(root_box, .Y)
	root_box.rect = {{0, 0}, {root_box.fixed_size[.X], root_box.fixed_size[.Y]}}
	layout_position(root_box)

	emit(root_box)

	if s.input.released[.Left] {
		s.active_key = 0
	}

	list: Draw_List
	list.commands = s.cmds
	list.mouse_over_ui = s.mouse_over || s.active_key != 0
	return list
}

////////////////////////////////
//~ fp: Widgets
//
// Each widget is a small set of calls to box. Use these functions for the
// usual cases.
//
// A button, a panel and a tooltip push their name as a tag around their box,
// so that the theme can address them. The tag of a panel covers its children.
// The sizes of those widgets are values in this file, and a caller can change
// them on the box that the function gives back.
//
// A label and a button take the size of their text. A wrapped text takes the
// width of its parent, and grows toward the bottom.
//
// Each pair of a begin and an end pushes the parent stack and pops it. The
// scoped form of each pair, below the pairs, runs its end when the scope of
// the caller ends.

label :: proc(str: string) {
	b := box({.Draw_Text}, str)
	b.pref_size[.X] = size_text(1)
	b.pref_size[.Y] = size_text(1)
}

labelf :: proc(format: string, args: ..any) {
	label(fmt.aprintf(format, ..args, allocator = state.frame_allocator))
}

text_wrapped :: proc(str: string) {
	b := box({.Draw_Text, .Text_Wrap}, str)
	b.pref_size[.X] = size_pct(1, 0)
	b.pref_size[.Y] = size_text(1)
}

button :: proc(str: string) -> Signal {
	push_tag("button")
	b := box({.Clickable, .Draw_Background, .Draw_Border, .Draw_Text}, str)
	pop_tag()
	b.pref_size[.X] = size_text(0)
	b.pref_size[.Y] = size_text(1)
	b.padding = {10, 5}
	b.corner_radius = 8
	b.border_thickness = 2
	b.text_align = .Center
	return signal(b)
}

// along the axis of the children of the parent
spacer :: proc(size: Size) {
	parent_box := stack_top(&state.parent_stack)
	b := box({}, "")
	off: Axis = (parent_box.child_axis == .X) ? .Y : .X
	b.pref_size[parent_box.child_axis] = size
	b.pref_size[off] = size_points(0, 0)
}

tooltip :: proc(str: string) {
	// The parent of a tooltip is the root box. floating_pos is relative to the
	// parent, and the position of the mouse is relative to the window, so the
	// origin of the root is the one origin that makes the two equal. The root
	// also keeps the tooltip outside the clip of each panel.
	push_parent(root())
	push_tag("tooltip")
	tip := box({.Draw_Background, .Draw_Border, .Draw_Text, .Floating}, str)
	pop_tag()
	pop_parent()
	tip.floating_pos = mouse() + geo.V2{16, 18}
	tip.padding = {8, 6}
	tip.corner_radius = 8
	tip.pref_size[.X] = size_text(1)
	tip.pref_size[.Y] = size_text(1)
}

// the children go along X
row_begin :: proc(str: string) -> ^Box {
	b := box({}, str)
	b.child_axis = .X
	push_parent(b)
	return b
}

row_end :: proc() {
	pop_parent()
}

// the children go along Y
column_begin :: proc(str: string) -> ^Box {
	b := box({}, str)
	b.child_axis = .Y
	push_parent(b)
	return b
}

column_end :: proc() {
	pop_parent()
}

// a column with a background, a border and a clip
panel_begin :: proc(str: string) -> ^Box {
	push_tag("panel") // It covers the children until panel_end.
	b := box({.Draw_Background, .Draw_Border, .Clip}, str)
	b.child_axis = .Y
	b.corner_radius = 10
	push_parent(b)
	return b
}

panel_end :: proc() {
	pop_parent()
	pop_tag()
}

//- fp: The scoped forms. Each one runs its end when the scope of the caller
//  ends, which is what the DeferLoop macros of the C build did.
//
//  Rule: a scoped call is the first statement of its scope. Style the widget
//  through the pointer it gives back, not with a push before the call: a box
//  stays writable until frame_end, so the two ways are equal.

@(deferred_none = pop_tag)
tag :: proc(t: string) {
	push_tag(t)
}

@(deferred_none = pop_parent)
parent :: proc(b: ^Box) {
	push_parent(b)
}

@(deferred_none = row_end)
row :: proc(str: string) -> ^Box {
	return row_begin(str)
}

@(deferred_none = column_end)
column :: proc(str: string) -> ^Box {
	return column_begin(str)
}

@(deferred_none = panel_end)
panel :: proc(str: string) -> ^Box {
	return panel_begin(str)
}

////////////////////////////////
//~ fp: Tests

// The tests run the module headless. The measure function below gives each
// character ten points, so a width in a test reads as a count of characters.
//
// The module holds one global, so two tests cannot run at one time. The lock
// at the start of each test makes the runner's threads take turns.

@(private) test_lock: sync.Mutex

@(private)
test_measure :: proc(user: rawptr, font: u64, size: f32, text: string) -> geo.V2 {
	return {f32(len(text)) * 10, size}
}

@(private)
test_metrics :: proc(user: rawptr, font: u64, size: f32) -> Font_Metrics {
	return {size * 0.8, size * 0.2, size}
}

@(private)
test_frame_begin :: proc(input: Input) {
	input := input
	if input.window == {} { input.window = {400, 300} }
	frame_begin(context.temp_allocator, input)
}

@(test)
layout_row_and_grow :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	test_frame_begin({})

	r := row_begin("row")
	r.pref_size[.X] = size_points(400, 1)
	r.pref_size[.Y] = size_points(20, 1)
	a := box({}, "a")
	a.pref_size[.X] = size_points(100, 1)
	a.pref_size[.Y] = size_points(20, 1)
	spacer(size_grow(1))
	b := box({}, "b")
	b.pref_size[.X] = size_points(50, 1)
	b.pref_size[.Y] = size_points(20, 1)
	row_end()

	frame_end()
	testing.expect_value(t, a.rect, geo.Rect{{0, 0}, {100, 20}})
	testing.expect_value(t, b.rect, geo.Rect{{350, 0}, {400, 20}})
}

@(test)
layout_text_and_labels :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	test_frame_begin({})

	label("abcd") // 40 points wide, 16 high at the default font size
	label("xy")

	frame_end()
	first := state.root.first
	second := first.next
	testing.expect_value(t, first.rect, geo.Rect{{0, 0}, {40, 16}})
	testing.expect_value(t, second.rect, geo.Rect{{0, 16}, {20, 32}})
}

@(test)
text_wrap_breaks_between_words :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	test_frame_begin({})

	b := box({.Draw_Text, .Text_Wrap}, "aaa bb cc")
	b.pref_size[.X] = size_points(55, 1)
	b.pref_size[.Y] = size_text(1)

	frame_end()
	testing.expect_value(t, len(b.lines), 2)
	testing.expect_value(t, b.lines[0], "aaa")
	testing.expect_value(t, b.lines[1], "bb cc")
	// two lines: one advance between the baselines, then ascent + descent
	testing.expect_value(t, b.fixed_size[.Y], f32(16 + 16))
}

@(test)
click_cycle_is_one_frame_delayed :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	over_button := geo.V2{10, 10}

	// Frame 1: the mouse stands over the place of the button, but the hover
	// reads the layout of the frame before, which does not exist yet.
	test_frame_begin({mouse = over_button})
	sig := button("Click###btn")
	testing.expect(t, !sig.hovered)
	frame_end()

	// Frame 2: the hover holds, and the press starts on the box.
	input2: Input
	input2.mouse = over_button
	input2.down[.Left] = true
	input2.pressed[.Left] = true
	test_frame_begin(input2)
	sig = button("Click###btn")
	testing.expect(t, sig.hovered)
	testing.expect(t, sig.pressed)
	testing.expect(t, !sig.clicked)
	frame_end()

	// Frame 3: the button comes up over the box, which is the click. The text
	// before "###" can change while the identity stays.
	input3: Input
	input3.mouse = over_button
	input3.released[.Left] = true
	test_frame_begin(input3)
	sig = button("Clack###btn")
	testing.expect(t, sig.hovered)
	testing.expect(t, sig.released)
	testing.expect(t, sig.clicked)
	frame_end()
}

@(test)
theme_most_tags_wins :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	BLUE :: geo.V4{0, 0, 1, 1}
	RED :: geo.V4{1, 0, 0, 1}
	patterns := []Theme_Pattern{
		{tags = {"background"}, color = BLUE},
		{tags = {"button", "background"}, color = RED},
	}
	set_theme({patterns = patterns})
	test_frame_begin({})
	defer frame_end()

	testing.expect_value(t, color_from_name("background"), BLUE)
	testing.expect_value(t, color_from_name("missing"), geo.V4{})
	{
		tag("button")
		testing.expect_value(t, color_from_name("background"), RED)
	}
	testing.expect_value(t, color_from_name("background"), BLUE)
}

@(test)
commands_come_in_draw_order :: proc(t: ^testing.T) {
	sync.guard(&test_lock)
	init(test_measure, test_metrics, nil)
	test_frame_begin({})

	{
		panel("p###panel")
		label("hi")
	}

	list := frame_end()
	// the panel: background, clip push, the text of the label, clip pop, border
	testing.expect_value(t, list.commands.count, 5)
	cmds := make([dynamic]^Draw_Command, context.temp_allocator)
	it := chunk.iterator(list.commands)
	for cmd in chunk.iterate(&it) { append(&cmds, cmd) }
	testing.expect_value(t, cmds[0].kind, Draw_Command_Kind.Rect)
	testing.expect_value(t, cmds[1].kind, Draw_Command_Kind.Clip_Push)
	testing.expect_value(t, cmds[2].kind, Draw_Command_Kind.Text)
	testing.expect_value(t, cmds[2].text, "hi")
	testing.expect_value(t, cmds[3].kind, Draw_Command_Kind.Clip_Pop)
	testing.expect_value(t, cmds[4].kind, Draw_Command_Kind.Rect)
	testing.expect_value(t, cmds[4].border_thickness, 1) // the default of the stack
}
