package thing

import "core:mem"
import "core:strings"

import "../../geo"

////////////////////////////////
//~ fp: Thing Database
//
// One contiguous structure of tables with a fixed size. A fact is a row, and
// the index of that row is a slot. A word and an edge are in a pool, and an
// index joins the parts of that pool. The structure holds no pointer, so you
// can move the whole Db, and you can save it as one block of bytes.
//
// All reads are total. A nil id and an old id both resolve to a shared nil
// object, which is read-only by convention. Never write through a nil object.
//
// This file holds two parts. The first part is the mechanism of the database:
// spawn and commit, iteration, words, and the accessors. The second part is
// the fact schema of the game, which is the members of the enums below:
// labels, flags, vars, fields and relations. Add a member to extend the
// schema. The mechanism does not read the meaning of a member.

// the largest number of things that can exist
THING_CAP :: 65000

// The largest size of the world. A field is a fact at each position, and each
// field column holds WORLD_MAX_DIM^2 cells, so the world can never become
// larger. World generation asserts its size against this limit.
WORLD_MAX_DIM :: 256
WORLD_CELLS :: WORLD_MAX_DIM * WORLD_MAX_DIM

@(private)
BITSET_WORDS :: (THING_CAP + 63) / 64
@(private)
WORD_CAP :: 8192
@(private)
WORD_HASH_SLOTS :: 16384 // power of two, > 2x WORD_CAP: never fills
@(private)
WORD_CHARS :: 256 * 1024
// The edges are in chunks. One chunk holds at most EDGE_CHUNK_LEN entries of
// one (rel, source) list. A walk of a list therefore reads one address for
// each chunk, and not one address for each edge. The link between two chunks
// is a u16 index, so a u16 sets the largest number of chunks. Chunk 0 is the
// nil chunk, which leaves the chunks 1 to 65535 for a list.
@(private)
EDGE_CHUNK_LEN :: 9
@(private)
EDGE_CHUNK_CAP :: 65536

#assert(WORD_HASH_SLOTS & (WORD_HASH_SLOTS - 1) == 0)
#assert(WORD_HASH_SLOTS >= 2 * WORD_CAP)

// A thing id holds a slot and a generation in one u32. Id 0 is always the nil
// id, and the database never gives it out. Test `id != 0` for a valid id, and
// use `==` to compare two ids. The numeric order of the ids is stable, which
// makes it useful for a sort and for nothing more.
Id :: distinct u32

////////////////////////////////
//~ fp: The Fact Schema

PHRASE_MAX_LEN :: 4

Word :: distinct u16

Phrase :: struct {
	len:   u8,
	words: [PHRASE_MAX_LEN]Word,
}

Label :: enum {
	None,
	Name,
}

// A flag is a boolean. The database holds each flag as a bitset. A flag is
// also a set of things: get is the test for a member, which costs one
// operation, and the iterators below walk the members.
Flag :: enum {
	None,
	Debug, // a mark for a test
	Player, // is this the player
	Placed, // stands on the board. The game gives it a pawn each tick.
	Mobile, // the thing can move
	Has_Influence, // claims the land near it. Each tile that it wins is its home.
}

// a variable is a scalar at each thing
Var :: enum {
	None,
	Move_Pts,
	Population,
	Food_Store, // the food in the granary now
	// The four numbers of the economy of the last tick. The tick writes them,
	// and the display reads them. Each one is a rate: the amount of one tick.
	Food_In, // the food that the land gave
	Food_Share, // the part of that food that went to the granary
	Food_Taken, // the part of that share that the granary held
	Food_Drawn, // the food that came out of the granary to feed the people
}

// An integer variable holds a tick count, an amount, or a value of an enum.
// Use it for each number that must stay exact above 2^24, which is the largest
// integer that an f32 holds exactly.
I_Var :: enum {
	None,
	X,
	Y,
	Sprite,
	Group_Type, // the row of the group type table. See the game package.
}

// A ref holds one target for each thing, for each kind of ref. Use it for a
// single reference such as an owner, a goal or a next. The database stores the
// direction from the thing to the target only. To find the things that point
// at a target, examine the refs of all the things.
Ref :: enum {
	None,
	Next,
	Goal,
}

// A relation holds many targets for each thing. Hold an amount of an item that
// has no state as an edge to a thing that stands for that kind of item, where
// the value of the edge is the amount. Hold an item that has a state of its
// own as a thing, and point at that thing with a ref.
Relation :: enum {
	None,
}

// A field is a fact at each position. It is the positional form of the
// families above. Each kind of field is one column, and the index of the
// column is a tile position and not a thing slot.
//
// A tile has no identity and no lifetime. Its position is its key.

// A field variable is a scalar at each position. The family holds the nil
// column only. Add a member to give each tile a new scalar.
Field :: enum {
	None,
}

// an integer field variable holds an id, a mask, or a value of an enum
I_Field :: enum {
	None,
	Terrain, // the id of a terrain type. See the terrain table of worldgen.
	// one connection mask for each defs.Feature, bit d = toward Dir4 d.
	// game.feature_ifield joins the two enums, and its switch is exhaustive.
	River_Mask,
	Road_Mask,
	// The groups that reach this tile. Each tick writes it again. A group that
	// draws from the tile divides the yield of the tile by this count.
	Claims,
}

// A field ref holds one thing at each position, for each kind of ref. A read
// examines the target, so a thing that the database removed reads back as nil.
Field_Ref :: enum {
	None,
	Home, // the group that the tile belongs to
}

// a field flag is a boolean at each position. The database holds it as a bitset.
Field_Flag :: enum {
	None,
}

////////////////////////////////
//~ fp: The Database

// The entries in a chunk have no order. To remove one entry, move the last
// entry into its place. EDGE_CHUNK_LEN makes the chunk fill one cache line
// exactly.
@(private)
Edge_Chunk :: struct #align (64) {
	values:  [EDGE_CHUNK_LEN]f32,
	targets: [EDGE_CHUNK_LEN]u16,
	next:    u16, // next chunk of the same (rel, source); freelist link while free
	rel:     u16, // Relation.None marks a free chunk
	source:  u16,
	count:   u8, // entries in use
}
#assert(size_of(Edge_Chunk) == 64)
#assert(EDGE_CHUNK_LEN <= 255)

Db :: struct {
	//- fp: the things. An iteration sees the slots in alive and not in nascent.
	//  A thing in doomed stays alive until commit removes it.
	alive:                [BITSET_WORDS]u64,
	nascent:              [BITSET_WORDS]u64,
	doomed:               [BITSET_WORDS]u64,
	generation:           [THING_CAP]u16,
	free_next:            [THING_CAP]u16, // freelist links through despawned slots
	first_free:           u16, // 0 = empty; slot 0 is never on the list
	watermark:            u16, // first never-used slot
	doomed_count:         u16, // marks since the last commit

	//- fp: the words. The database stores each word one time and never frees
	//  it. Word 0 is the nil word.
	word_offset:          [WORD_CAP]u32,
	word_len:             [WORD_CAP]u8,
	word_hash:            [WORD_HASH_SLOTS]u16, // open-addressed word ids; 0 = empty
	word_chars:           [WORD_CHARS]u8,
	word_count:           u32,
	word_chars_used:      u32,

	//- fp: the facts, as table[kind][slot]. The rows of kind None stay empty,
	//  so a kind is a direct index. A row that no code writes costs pages of
	//  zeros only.
	labels:               [Label][THING_CAP]Phrase,
	flags:                [Flag][BITSET_WORDS]u64,
	vars:                 [Var][THING_CAP]f32,
	ivars:                [I_Var][THING_CAP]i32,
	refs:                 [Ref][THING_CAP]Id,

	//- fp: the edges, in a pool. Each (rel, source) has a list of chunks, and
	//  an index joins them. Chunk 0 is the nil chunk, so 0 ends a list. The
	//  alignment puts each chunk on its own cache line, which is the reason for
	//  the chunks.
	edge_chunks:          [EDGE_CHUNK_CAP]Edge_Chunk,
	edge_first:           [Relation][THING_CAP]u16,
	edge_chunk_free:      u16,
	edge_chunk_watermark: u32, // counts to EDGE_CHUNK_CAP, so it outgrows a u16

	//- fp: the fields, as table[kind][cell], where cell = y * WORLD_MAX_DIM +
	//  x. Each column holds WORLD_CELLS cells. world_width and world_height
	//  give the part in use. The rows of kind None stay empty, as in the
	//  tables of the things.
	world_width:          int,
	world_height:         int,
	fields:               [Field][WORLD_CELLS]f32,
	ifields:              [I_Field][WORLD_CELLS]i32,
	field_refs:           [Field_Ref][WORLD_CELLS]Id,
	field_flags:          [Field_Flag][WORLD_CELLS / 64]u64,
}

////////////////////////////////
//~ fp: Nil

nil_phrase: Phrase
nil_var: f32
nil_ivar: i32

////////////////////////////////
//~ fp: Internals

@(private)
bit_get :: proc(bits: []u64, idx: int) -> bool {
	return (bits[idx >> 6] >> uint(idx & 63)) & 1 != 0
}

@(private)
bit_write :: proc(bits: []u64, idx: int, value: bool) {
	mask := u64(1) << uint(idx & 63)
	if value {
		bits[idx >> 6] |= mask
	} else {
		bits[idx >> 6] &~= mask
	}
}

// An id holds (slot << 16) | generation, so the numeric order of two ids
// follows their slots first.

// The live slot of an id. The slot must be alive now, and its generation must
// be equal to the generation of the id. The result is 0 for a nil id and for
// an old id.
@(private)
live_slot :: proc(db: ^Db, id: Id) -> int {
	s := int(u32(id) >> 16)
	if s == 0 || s >= THING_CAP {return 0}
	if !bit_get(db.alive[:], s) {return 0}
	if db.generation[s] != u16(u32(id) & 0xFFFF) {return 0}
	return s
}

@(private)
id_from_slot :: proc(db: ^Db, s: int) -> Id {
	return Id(u32(s) << 16 | u32(db.generation[s]))
}

// The first slot at `start` or after it that an iteration sees, which is a
// slot in alive and not in nascent. The result is nil when there is no such
// slot.
@(private)
scan_from :: proc(db: ^Db, start: int) -> Id {
	for s := start; s < int(db.watermark); {
		word := (db.alive[s >> 6] &~ db.nascent[s >> 6]) >> uint(s & 63)
		if word == 0 {
			s = (s &~ 63) + 64
			continue
		}
		if word & 1 != 0 {return id_from_slot(db, s)}
		s += 1
	}
	return 0
}

////////////////////////////////
//~ fp: Lifetime

init :: proc(allocator := context.allocator) -> ^Db {
	db := new(Db, allocator)
	db.watermark = 1 // slot 0 is the nil thing
	db.word_count = 1 // word 0 is the nil word
	db.edge_chunk_watermark = 1 // chunk 0 is the nil entry
	return db
}

// A thing that you spawn exists at once: its id is valid, and you can write to
// it. An iteration does not see the thing until commit. Despawn puts a mark on
// the thing. The thing stays live until commit removes it: commit then drops
// its edges, sets its rows to 0, and makes its id old.
spawn :: proc(db: ^Db) -> Id { 	// nil when all THING_CAP slots are live
	s := int(db.first_free)
	if s != 0 {
		db.first_free = db.free_next[s]
	} else if int(db.watermark) < THING_CAP {
		s = int(db.watermark)
		db.watermark += 1
	} else {
		return 0
	}
	bit_write(db.alive[:], s, true)
	bit_write(db.nascent[:], s, true)
	return id_from_slot(db, s)
}

despawn_mark :: proc(db: ^Db, id: Id) {
	s := live_slot(db, id)
	if s != 0 && !bit_get(db.doomed[:], s) {
		bit_write(db.doomed[:], s, true)
		db.doomed_count += 1
	}
}

commit :: proc(db: ^Db) {
	if db.doomed_count > 0 {
		//- fp: build the edge lists again from the pool, and drop each edge
		//  that touches a doomed thing. This one sweep is the reason that an
		//  edge can store one direction only.
		//
		//  A chunk whose source is gone goes back to the free list whole. In
		//  each other chunk the sweep removes the targets that are gone, and it
		//  frees that chunk only when it becomes empty.
		db.edge_first = {}
		db.edge_chunk_free = 0
		for c in 1 ..< int(db.edge_chunk_watermark) {
			edge_chunk := &db.edge_chunks[c]
			if edge_chunk.rel == 0 || bit_get(db.doomed[:], int(edge_chunk.source)) {
				edge_chunk.count = 0
			} else {
				for i := 0; i < int(edge_chunk.count); {
					if bit_get(db.doomed[:], int(edge_chunk.targets[i])) {
						edge_chunk.count -= 1
						edge_chunk.targets[i] = edge_chunk.targets[edge_chunk.count]
						edge_chunk.values[i] = edge_chunk.values[edge_chunk.count]
					} else {
						i += 1
					}
				}
			}
			if edge_chunk.count == 0 {
				edge_chunk.rel = 0
				edge_chunk.next = db.edge_chunk_free
				db.edge_chunk_free = u16(c)
			} else {
				edge_chunk.next = db.edge_first[Relation(edge_chunk.rel)][edge_chunk.source]
				db.edge_first[Relation(edge_chunk.rel)][edge_chunk.source] = u16(c)
			}
		}
		//- fp: remove each doomed thing. Set each of its fact rows to 0, make
		//  its id old, and put its slot on the free list.
		for word_idx in 0 ..< BITSET_WORDS {
			for word := db.doomed[word_idx]; word != 0; {
				bit: uint = 0
				for word & (1 << bit) == 0 {bit += 1}
				word &~= 1 << bit
				s := word_idx * 64 + int(bit)
				for label_kind in Label {if label_kind != .None {db.labels[label_kind][s] = {}}}
				for flag in Flag {if flag != .None {bit_write(db.flags[flag][:], s, false)}}
				for v in Var {if v != .None {db.vars[v][s] = 0}}
				for iv in I_Var {if iv != .None {db.ivars[iv][s] = 0}}
				for r in Ref {if r != .None {db.refs[r][s] = 0}}
				db.generation[s] += 1
				bit_write(db.alive[:], s, false)
				db.free_next[s] = db.first_free
				db.first_free = u16(s)
			}
		}
		db.doomed = {}
		db.doomed_count = 0
	}
	//- fp: an iteration now sees each thing that a spawn made
	db.nascent = {}
}

////////////////////////////////
//~ fp: Iteration
//
// The protocol is construct-free: first gives the first live thing, and next
// gives the one after `id`. A result of 0 shows that the iteration is
// complete.

first :: proc(db: ^Db) -> Id {
	return scan_from(db, 1)
}

next :: proc(db: ^Db, id: Id) -> Id {
	if id == 0 {return 0}
	return scan_from(db, int(u32(id) >> 16) + 1)
}

// The iterator over the things with `flag`. Its protocol is the protocol of
// first and next.
first_flagged :: proc(db: ^Db, flag: Flag) -> Id {
	id := first(db)
	for id != 0 && !flag_get(db, id, flag) {id = next(db, id)}
	return id
}

next_flagged :: proc(db: ^Db, flag: Flag, id: Id) -> Id {
	id := next(db, id)
	for id != 0 && !flag_get(db, id, flag) {id = next(db, id)}
	return id
}

////////////////////////////////
//~ fp: Words

@(private)
word_str :: proc(db: ^Db, word: Word) -> string {
	return string(db.word_chars[db.word_offset[int(word)]:][:db.word_len[int(word)]])
}

@(private)
hash_str :: proc(s: string) -> u64 {
	hash: u64 = 0xcbf29ce484222325 // fnv-1a
	for idx in 0 ..< len(s) {
		hash = (hash ~ u64(s[idx])) * 0x100000001b3
	}
	return hash
}

define_word :: proc(db: ^Db, source: string) -> Word {
	if len(source) == 0 || len(source) > 255 {return 0}
	mask := u32(WORD_HASH_SLOTS - 1)
	for probe := u32(hash_str(source)) & mask;; probe = (probe + 1) & mask {
		word := db.word_hash[probe]
		if word == 0 {
			if db.word_count >= WORD_CAP {return 0}
			if int(db.word_chars_used) + len(source) > WORD_CHARS {return 0}
			w := Word(db.word_count)
			db.word_count += 1
			db.word_offset[int(w)] = db.word_chars_used
			db.word_len[int(w)] = u8(len(source))
			copy(db.word_chars[db.word_chars_used:], source)
			db.word_chars_used += u32(len(source))
			db.word_hash[probe] = u16(w)
			return w
		}
		if word_str(db, Word(word)) == source {return Word(word)}
	}
}

resolve_word :: proc(db: ^Db, word: Word, fallback: string) -> string {
	if word == 0 || u32(word) >= db.word_count {return fallback}
	return word_str(db, word)
}

push_word :: proc(phrase: ^Phrase, word: Word) -> bool { 	// false when full
	if int(phrase.len) >= PHRASE_MAX_LEN {return false}
	phrase.words[phrase.len] = word
	phrase.len += 1
	return true
}

resolve_phrase :: proc(
	db: ^Db,
	phrase: Phrase,
	fallback: string,
	allocator := context.allocator,
) -> string {
	parts: [PHRASE_MAX_LEN]string
	n := 0
	l := min(int(phrase.len), PHRASE_MAX_LEN)
	for idx in 0 ..< l {
		word := resolve_word(db, phrase.words[idx], "")
		if len(word) > 0 {parts[n] = word; n += 1}
	}
	if n == 0 {return fallback}
	return strings.join(parts[:n], " ", allocator)
}

////////////////////////////////
//~ fp: Labels

label :: proc(db: ^Db, id: Id, label_kind: Label) -> ^Phrase {
	s := live_slot(db, id)
	if s == 0 || label_kind == .None {return &nil_phrase}
	return &db.labels[label_kind][s]
}

////////////////////////////////
//~ fp: Flags

flag_get :: proc(db: ^Db, id: Id, flag: Flag) -> bool {
	s := live_slot(db, id)
	if s == 0 || flag == .None {return false}
	return bit_get(db.flags[flag][:], s)
}

// set gives you the value that the flag had before
flag_set :: proc(db: ^Db, id: Id, flag: Flag, value: bool) -> bool {
	s := live_slot(db, id)
	if s == 0 || flag == .None {return false}
	old := bit_get(db.flags[flag][:], s)
	bit_write(db.flags[flag][:], s, value)
	return old
}

////////////////////////////////
//~ fp: Variables

var :: proc(db: ^Db, id: Id, v: Var) -> ^f32 {
	s := live_slot(db, id)
	if s == 0 || v == .None {return &nil_var}
	return &db.vars[v][s]
}

var_get :: proc(db: ^Db, id: Id, v: Var) -> f32 {
	return var(db, id, v)^
}

// a safer write than var(...)^ = ... : it does nothing for a nil id
var_set :: proc(db: ^Db, id: Id, v: Var, value: f32) {
	p := var(db, id, v)
	if p != &nil_var {p^ = value}
}

ivar :: proc(db: ^Db, id: Id, iv: I_Var) -> ^i32 {
	s := live_slot(db, id)
	if s == 0 || iv == .None {return &nil_ivar}
	return &db.ivars[iv][s]
}

ivar_get :: proc(db: ^Db, id: Id, iv: I_Var) -> i32 {
	return ivar(db, id, iv)^
}

// a safer write than ivar(...)^ = ... : it does nothing for a nil id
ivar_set :: proc(db: ^Db, id: Id, iv: I_Var, value: i32) {
	p := ivar(db, id, iv)
	if p != &nil_ivar {p^ = value}
}

////////////////////////////////
//~ fp: Refs

ref_get :: proc(db: ^Db, ref: Ref, id: Id) -> Id {
	s := live_slot(db, id)
	if s == 0 || ref == .None {return 0}
	target := db.refs[ref][s]
	if live_slot(db, target) == 0 {return 0} 	// target despawned since
	return target
}

ref_set :: proc(db: ^Db, ref: Ref, id: Id, target: Id) {
	target := target
	s := live_slot(db, id)
	if s == 0 || ref == .None {return}
	if live_slot(db, target) == 0 {target = 0} 	// nil / stale clears
	db.refs[ref][s] = target
}

////////////////////////////////
//~ fp: Relations

Edge_Entry :: struct {
	id:    Id,
	value: f32,
}

// the value of one edge of a list. It gives `fallback` when the edge is absent.
edge_value :: proc(edges_list: []Edge_Entry, id: Id, fallback: f32) -> f32 {
	for &entry in edges_list {
		if entry.id == id {return entry.value}
	}
	return fallback
}

// each edge that goes out of `source` under `rel`, pushed on `allocator`
edges :: proc(db: ^Db, rel: Relation, source: Id, allocator := context.allocator) -> []Edge_Entry {
	s := live_slot(db, source)
	if s == 0 || rel == .None {return nil}
	count := 0
	for c := db.edge_first[rel][s]; c != 0; c = db.edge_chunks[c].next {
		count += int(db.edge_chunks[c].count)
	}
	result := make([]Edge_Entry, count, allocator)
	n := 0
	for c := db.edge_first[rel][s]; c != 0; c = db.edge_chunks[c].next {
		edge_chunk := &db.edge_chunks[c]
		for i in 0 ..< int(edge_chunk.count) {
			result[n] = {id_from_slot(db, int(edge_chunk.targets[i])), edge_chunk.values[i]}
			n += 1
		}
	}
	return result
}

// The value of one edge, without a list. It gives `fallback` when the edge is
// absent.
edge_get :: proc(db: ^Db, rel: Relation, source: Id, target: Id, fallback: f32) -> f32 {
	src := live_slot(db, source)
	tgt := live_slot(db, target)
	if src == 0 || tgt == 0 || rel == .None {return fallback}
	for c := db.edge_first[rel][src]; c != 0; c = db.edge_chunks[c].next {
		edge_chunk := &db.edge_chunks[c]
		for i in 0 ..< int(edge_chunk.count) {
			if int(edge_chunk.targets[i]) == tgt {return edge_chunk.values[i]}
		}
	}
	return fallback
}

// A relation is a matrix. A set with a value of 0 removes the edge.
edge_set :: proc(db: ^Db, rel: Relation, source: Id, target: Id, value: f32) {
	src := live_slot(db, source)
	tgt := live_slot(db, target)
	if src == 0 || tgt == 0 || rel == .None {return}
	//- fp: an edge that exists. Write its value, or remove it when the value
	//  is 0 by a move of the last entry into its place.
	//
	//  `link` stays one step behind, at the field that points to the chunk in
	//  hand. A chunk that becomes empty at any place in the list therefore
	//  leaves the list with one store, and the list needs no backward link.
	room: u16 = 0 // a chunk of this list with a spare entry, for the insert below
	for link := &db.edge_first[rel][src]; link^ != 0; link = &db.edge_chunks[link^].next {
		c := link^
		edge_chunk := &db.edge_chunks[c]
		if int(edge_chunk.count) < EDGE_CHUNK_LEN {room = c}
		for i in 0 ..< int(edge_chunk.count) {
			if int(edge_chunk.targets[i]) != tgt {continue}
			if value != 0 {
				edge_chunk.values[i] = value
				return
			}
			edge_chunk.count -= 1
			edge_chunk.targets[i] = edge_chunk.targets[edge_chunk.count]
			edge_chunk.values[i] = edge_chunk.values[edge_chunk.count]
			if edge_chunk.count == 0 {
				link^ = edge_chunk.next
				edge_chunk.rel = 0
				edge_chunk.next = db.edge_chunk_free
				db.edge_chunk_free = c
			}
			return
		}
	}
	if value == 0 {return}
	//- fp: a new edge. Use a free place that an earlier removal left, before
	//  you take a new chunk.
	if room != 0 {
		edge_chunk := &db.edge_chunks[room]
		edge_chunk.targets[edge_chunk.count] = u16(tgt)
		edge_chunk.values[edge_chunk.count] = value
		edge_chunk.count += 1
		return
	}
	//- fp: a new chunk. An empty pool means that EDGE_CHUNK_CAP is too small,
	//  so an assert stops the program.
	c := db.edge_chunk_free
	if c != 0 {
		db.edge_chunk_free = db.edge_chunks[c].next
	} else if db.edge_chunk_watermark < EDGE_CHUNK_CAP {
		c = u16(db.edge_chunk_watermark)
		db.edge_chunk_watermark += 1
	} else {
		panic("edge chunk pool exhausted")
	}
	edge_chunk := &db.edge_chunks[c]
	edge_chunk^ = {
		rel    = u16(rel),
		source = u16(src),
		count  = 1,
		next   = db.edge_first[rel][src],
	}
	edge_chunk.targets[0] = u16(tgt)
	edge_chunk.values[0] = value
	db.edge_first[rel][src] = c
}

////////////////////////////////
//~ fp: Fields
//
// World generation sets the size of the world one time, and each axis is at
// most WORLD_MAX_DIM. That size limits each read and each write. A position
// outside the world resolves to a shared nil object, which is read-only by
// convention.

world_size_set :: proc(db: ^Db, width, height: int) {
	assert(1 <= width && width <= WORLD_MAX_DIM)
	assert(1 <= height && height <= WORLD_MAX_DIM)
	db.world_width = width
	db.world_height = height
}

world_size :: proc(db: ^Db) -> geo.V2i {
	return {db.world_width, db.world_height}
}

world_in_bounds :: proc(db: ^Db, pos: geo.V2i) -> bool {
	return 0 <= pos.x && pos.x < db.world_width && 0 <= pos.y && pos.y < db.world_height
}

@(private)
cell :: proc(pos: geo.V2i) -> int {
	return pos.y * WORLD_MAX_DIM + pos.x
}

field :: proc(db: ^Db, pos: geo.V2i, f: Field) -> ^f32 {
	if !world_in_bounds(db, pos) || f == .None {return &nil_var}
	return &db.fields[f][cell(pos)]
}

field_get :: proc(db: ^Db, pos: geo.V2i, f: Field) -> f32 {
	return field(db, pos, f)^
}

// a safer write than field(...)^ = ... : it does nothing outside the world
field_set :: proc(db: ^Db, pos: geo.V2i, f: Field, value: f32) {
	p := field(db, pos, f)
	if p != &nil_var {p^ = value}
}

ifield :: proc(db: ^Db, pos: geo.V2i, f: I_Field) -> ^i32 {
	if !world_in_bounds(db, pos) || f == .None {return &nil_ivar}
	return &db.ifields[f][cell(pos)]
}

ifield_get :: proc(db: ^Db, pos: geo.V2i, f: I_Field) -> i32 {
	return ifield(db, pos, f)^
}

// a safer write than ifield(...)^ = ... : it does nothing outside the world
ifield_set :: proc(db: ^Db, pos: geo.V2i, f: I_Field, value: i32) {
	p := ifield(db, pos, f)
	if p != &nil_ivar {p^ = value}
}

// Set every cell of one column to 0, which is its nil value (ZII). The write
// covers the whole column, past the world too. A cell outside the world is
// unreachable by a read, and it holds 0 already, so this is correct and stays
// one operation.
ifield_clear :: proc(db: ^Db, f: I_Field) {
	if f == .None {return}
	db.ifields[f] = {}
}

// Read and write one bit of a mask field. The index of a bit is 0 to 31. Set
// gives you the value that the bit had before, as flag_set does, and it does
// nothing outside the world.
ifield_get_bit :: proc(db: ^Db, pos: geo.V2i, f: I_Field, bit: uint) -> bool {
	if bit >= 32 {return false}
	return (ifield_get(db, pos, f) >> bit) & 1 != 0
}

ifield_set_bit :: proc(db: ^Db, pos: geo.V2i, f: I_Field, bit: uint, value: bool) -> bool {
	p := ifield(db, pos, f)
	if p == &nil_ivar || bit >= 32 {return false}
	old := (p^ >> bit) & 1 != 0
	if value {
		p^ |= i32(1 << bit)
	} else {
		p^ &~= i32(1 << bit)
	}
	return old
}

field_ref_get :: proc(db: ^Db, ref: Field_Ref, pos: geo.V2i) -> Id {
	if !world_in_bounds(db, pos) || ref == .None {return 0}
	target := db.field_refs[ref][cell(pos)]
	if live_slot(db, target) == 0 {return 0} 	// target despawned since
	return target
}

field_ref_set :: proc(db: ^Db, ref: Field_Ref, pos: geo.V2i, target: Id) {
	target := target
	if !world_in_bounds(db, pos) || ref == .None {return}
	if live_slot(db, target) == 0 {target = 0} 	// nil / stale clears
	db.field_refs[ref][cell(pos)] = target
}

// set every cell of the column to the nil id, as ifield_clear does
field_ref_clear :: proc(db: ^Db, ref: Field_Ref) {
	if ref == .None {return}
	db.field_refs[ref] = {}
}

field_flag_get :: proc(db: ^Db, pos: geo.V2i, flag: Field_Flag) -> bool {
	if !world_in_bounds(db, pos) || flag == .None {return false}
	return bit_get(db.field_flags[flag][:], cell(pos))
}

// set gives you the value that the flag had before
field_flag_set :: proc(db: ^Db, pos: geo.V2i, flag: Field_Flag, value: bool) -> bool {
	if !world_in_bounds(db, pos) || flag == .None {return false}
	c := cell(pos)
	old := bit_get(db.field_flags[flag][:], c)
	bit_write(db.field_flags[flag][:], c, value)
	return old
}

