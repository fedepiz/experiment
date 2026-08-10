#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/strings.h"
#include "base/arena.h"
#include "game/defs.h"

// the largest number of things that can exist
#define TH_THING_CAP 65000

// The largest size of the world. A field is a fact at each position, and each
// field column holds TH_WORLD_MAX_DIM^2 cells, so the world can never become
// larger. World generation asserts its size against this limit.
#define TH_WORLD_MAX_DIM 256
#define TH_WORLD_CELLS   (TH_WORLD_MAX_DIM * TH_WORLD_MAX_DIM)

// The database of things. Keep it contiguous, and keep it possible to move:
// store no pointers in it. You can then save it and load it as one block of
// bytes.
typedef struct TH_Db TH_Db;

// A thing id holds a slot and a generation in one U32. Id 0 is always the nil
// id, and the database never gives it out. Test `id != 0` for a valid id, and
// use `==` to compare two ids. The numeric order of the ids is stable, which
// makes it useful for a sort and for nothing more.
typedef U32 TH_Id;

// All reads are total. A nil id and an old id both resolve to a shared nil
// object, which is read-only by convention. Never write through a nil object.
//
// This file holds two parts. The first part is the mechanism of the database:
// spawn and commit, iteration, words, and the accessors. The second part is
// the fact schema of the game, which is the members of the enums below:
// labels, flags, vars, fields and relations. Add a member to extend the
// schema. The mechanism does not read the meaning of a member.

internal TH_Db* th_init_db(Arena* arena);

// A thing that you spawn exists at once: its id is valid, and you can write to
// it. An iteration does not see the thing until th_commit. Despawn puts a mark
// on the thing. The thing stays live until th_commit removes it: th_commit
// then drops its edges, sets its rows to 0, and makes its id old.
internal TH_Id th_spawn(TH_Db* db); // nil when all TH_THING_CAP slots are live
internal void th_despawn_mark(TH_Db* db, TH_Id id);
internal void th_commit(TH_Db* db);

// The iterator over the live things. A result of 0 shows that the iteration is
// complete.
internal TH_Id th_first(TH_Db* db);
internal TH_Id th_next(TH_Db* db, TH_Id id);

#define TH_PHRASE_MAX_LEN 4

typedef U16 TH_Word;

internal TH_Word th_define_word(TH_Db* db, String8 source);
internal String8 th_resolve_word(TH_Db* db, TH_Word word, String8 fallback);

typedef struct {
  U8 len;
  TH_Word words[TH_PHRASE_MAX_LEN];
} TH_Phrase;

internal B32 th_push_word(TH_Phrase* phrase, TH_Word word); // false when full
internal String8 th_resolve_phrase(Arena* out, TH_Db* db, TH_Phrase phrase, String8 fallback);

typedef U16 TH_Label;

enum {
  TH_Label_Nil,
  TH_Label_Name,
  TH_Label_COUNT,
};

internal TH_Phrase* th_label(TH_Db* db, TH_Id id, TH_Label label);

// A flag is a boolean. The database holds each flag as a bitset. A flag is
// also a set of things: get is the test for a member, which costs one
// operation, and the iterators below walk the members.

typedef U8 TH_Flag;
enum {
  TH_Flag_Nil,
  TH_Flag_Debug,        // a mark for a test
  TH_Flag_Placed,       // stands on the board. The game gives it a pawn each tick.
  TH_Flag_Mobile,       // the thing can move
  TH_Flag_HasInfluence, // claims the land near it. Each tile that it wins is its home.
  TH_Flag_COUNT,
};

internal B32 th_flag_get(TH_Db* db, TH_Id id, TH_Flag flag);
// set gives you the value that the flag had before
internal B32 th_flag_set(TH_Db* db, TH_Id id, TH_Flag flag, B32 value);

// The iterator over the things with `flag`. Its protocol is the protocol of
// th_first and th_next.
internal TH_Id th_first_flagged(TH_Db* db, TH_Flag flag);
internal TH_Id th_next_flagged(TH_Db* db, TH_Flag flag, TH_Id id);

// a variable is a scalar at each thing
typedef U16 TH_Var;
enum {
  TH_Var_Nil,
  TH_Var_MovePts,
  TH_Var_Population,
  TH_Var_FoodStore, // the food in the granary now
  // The four numbers of the economy of the last tick. The tick writes them,
  // and the display reads them. Each one is a rate: the amount of one tick.
  TH_Var_FoodIn,    // the food that the land gave
  TH_Var_FoodShare, // the part of that food that went to the granary
  TH_Var_FoodTaken, // the part of that share that the granary held
  TH_Var_FoodDrawn, // the food that came out of the granary to feed the people
  TH_Var_COUNT
};

internal F32* th_var(TH_Db* db, TH_Id id, TH_Var var);
internal F32 th_var_get(TH_Db* db, TH_Id id, TH_Var var);
// a safer write than *th_var() = ... : it does nothing for a nil id
internal void th_var_set(TH_Db* db, TH_Id id, TH_Var var, F32 value);

// An integer variable holds a tick count, an amount, or a value of an enum.
// Use it for each number that must stay exact above 2^24, which is the largest
// integer that an F32 holds exactly.
typedef U16 TH_IVar;
enum {
  TH_IVar_Nil,
  TH_IVar_X,
  TH_IVar_Y,
  TH_IVar_Sprite,
  TH_IVar_GroupType, // the row of the group type table. See game.h.
  TH_IVar_COUNT
};

internal I32* th_ivar(TH_Db* db, TH_Id id, TH_IVar ivar);
internal I32 th_ivar_get(TH_Db* db, TH_Id id, TH_IVar ivar);
// a safer write than *th_ivar() = ... : it does nothing for a nil id
internal void th_ivar_set(TH_Db* db, TH_Id id, TH_IVar ivar, I32 value);

// A ref holds one target for each thing, for each kind of ref. Use it for a
// single reference such as an owner, a goal or a next. The database stores the
// direction from the thing to the target only. To find the things that point
// at a target, examine the refs of all the things.
typedef U16 TH_Ref;
enum {
  TH_Ref_Nil,
  TH_Ref_Next,
  TH_Ref_Goal,
  TH_Ref_COUNT,
};

internal TH_Id th_ref_get(TH_Db* db, TH_Ref ref, TH_Id thing);
internal void th_ref_set(TH_Db* db, TH_Ref ref, TH_Id thing, TH_Id target);

// A relation holds many targets for each thing. Hold an amount of an item that
// has no state as an edge to a thing that stands for that kind of item, where
// the value of the edge is the amount. Hold an item that has a state of its
// own as a thing, and point at that thing with a ref.
typedef U16 TH_Relation;
enum {
  TH_Relation_Nil,
  TH_Relation_COUNT
};

typedef struct {
  TH_Id id;
  F32 value;
} TH_EdgeEntry;

typedef struct {
  U64 count;
  TH_EdgeEntry* entries;
} TH_Edges;

// the value of one edge of a list. It gives `fallback` when the edge is absent.
internal F32 th_edge_value(TH_Edges edges, TH_Id id, F32 fallback);

// each edge that goes out of `source` under `rel`, pushed on `arena`
internal TH_Edges th_edges(Arena* arena, TH_Db* db, TH_Relation rel, TH_Id source);

// The value of one edge, without a list. It gives `fallback` when the edge is
// absent.
internal F32 th_edge_get(TH_Db* db, TH_Relation rel, TH_Id source, TH_Id target, F32 fallback);

// A relation is a matrix. A set with a value of 0 removes the edge.
internal void th_edge_set(TH_Db* db, TH_Relation rel, TH_Id source, TH_Id target, F32 value);

// A field is a fact at each position. It is the positional form of the
// families above. Each kind of field is one column, and the index of the
// column is a tile position and not a thing slot.
//
// A tile has no identity and no lifetime. Its position is its key.
//
// World generation sets the size of the world one time, and each axis is at
// most TH_WORLD_MAX_DIM. That size limits each read and each write. A position
// outside the world resolves to a shared nil object, which is read-only by
// convention.

internal void th_world_size_set(TH_Db* db, I32 width, I32 height);
internal V2I th_world_size(TH_Db* db);
internal B32 th_world_in_bounds(TH_Db* db, V2I pos);

// A field variable is a scalar at each position. The family holds the nil
// column only. Add a member to give each tile a new scalar.
typedef U16 TH_Field;
enum {
  TH_Field_Nil,
  TH_Field_COUNT
};
StaticAssert(TH_Field_Nil == 0, th_field_nil_first);

internal F32* th_field(TH_Db* db, V2I pos, TH_Field field);
internal F32 th_field_get(TH_Db* db, V2I pos, TH_Field field);
// a safer write than *th_field() = ... : it does nothing outside the world
internal void th_field_set(TH_Db* db, V2I pos, TH_Field field, F32 value);

// an integer field variable holds an id, a mask, or a value of an enum
typedef U16 TH_IField;
enum {
  TH_IField_Nil,
  TH_IField_Terrain, // the id of a terrain type. See the terrain table of worldgen.
// one connection mask for each feature (defs.h), bit d = toward Dir4 d
#define X(name, key) TH_IField_##name##Mask,
  DF_FEATURE_LIST
#undef X
      // The groups that reach this tile. Each tick writes it again. A group that
      // draws from the tile divides the yield of the tile by this count.
      TH_IField_Claims,
  TH_IField_COUNT
};
StaticAssert(TH_IField_Nil == 0, th_ifield_nil_first);

internal I32* th_ifield(TH_Db* db, V2I pos, TH_IField ifield);
internal I32 th_ifield_get(TH_Db* db, V2I pos, TH_IField ifield);
// a safer write than *th_ifield() = ... : it does nothing outside the world
internal void th_ifield_set(TH_Db* db, V2I pos, TH_IField ifield, I32 value);

// Read and write one bit of a mask field. The index of a bit is 0 to 31. Set
// gives you the value that the bit had before, as th_flag_set does, and it
// does nothing outside the world.
internal B32 th_ifield_get_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit);
internal B32 th_ifield_set_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit, B32 value);

// A field ref holds one thing at each position, for each kind of ref. A read
// examines the target, so a thing that the database removed reads back as nil.
typedef U16 TH_FieldRef;
enum {
  TH_FieldRef_Nil,
  TH_FieldRef_Home, // the group that the tile belongs to
  TH_FieldRef_COUNT
};

internal TH_Id th_field_ref_get(TH_Db* db, TH_FieldRef ref, V2I pos);
internal void th_field_ref_set(TH_Db* db, TH_FieldRef ref, V2I pos, TH_Id target);

// a field flag is a boolean at each position. The database holds it as a bitset.
typedef U8 TH_FieldFlag;
enum {
  TH_FieldFlag_Nil,
  TH_FieldFlag_COUNT
};

internal B32 th_field_flag_get(TH_Db* db, V2I pos, TH_FieldFlag flag);
// set gives you the value that the flag had before
internal B32 th_field_flag_set(TH_Db* db, V2I pos, TH_FieldFlag flag, B32 value);
