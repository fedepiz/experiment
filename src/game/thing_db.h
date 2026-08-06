#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/strings.h"
#include "base/arena.h"

// Max number of things that can possibly exist
#define TH_NUM_THINGS 65000

// Max world dimensions: fields (per-position facts, below) are fixed columns
// of TH_WORLD_MAX_DIM^2 cells, so the world can never outgrow them. World
// generation asserts its dimensions against this.
#define TH_WORLD_MAX_DIM 256
#define TH_WORLD_CELLS (TH_WORLD_MAX_DIM * TH_WORLD_MAX_DIM)

// The structure that contains the database of things.
// KEEP CONTIGUOUS and relocatable (ie, no stored pointers)
// This way we can save and restore by binary blobs
typedef struct TH_Db TH_Db;

// A thing id: slot and generation packed into one U32. 0 is guaranteed to be
// the NIL id, never given out, so `id != 0` tests validity and `==` compares.
// Numeric order is a stable total order for sorting and nothing more.
typedef U32 TH_Id;

// Reads are total: nil and stale ids resolve to shared nil objects, read-only
// by convention -- nothing may ever write through a nil.

internal TH_Db* th_init_db(Arena* arena);

// A spawned thing exists at once -- its id is valid and writable -- but stays
// invisible to iteration until th_commit. Despawn only marks: the thing stays
// live until th_commit destroys it (edges dropped, rows zeroed, id stale).
internal TH_Id th_spawn(TH_Db* db); // nil when all TH_NUM_THINGS slots are live
internal void th_despawn_mark(TH_Db* db, TH_Id id);
internal void th_commit(TH_Db* db);

// Iterator over live things. When th_first/th_next return 0, we are done
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

// Flags are booleans, implemented as a bitset. A flag doubles as a set of
// things: get is the O(1) membership test, and the flagged iterators below
// walk the members.

typedef U8 TH_Flag;
enum {
  TH_Flag_Nil,
  TH_Flag_Debug, // A test mark
  TH_Flag_Placed, // standing on the board: reconciled to a pawn each tick
  TH_Flag_HasInfluence, // claims the land around it; tiles it wins are homed to it
  TH_Flag_COUNT,
};

internal B32 th_flag_get(TH_Db* db, TH_Id id, TH_Flag flag);
// Return the old value
internal B32 th_flag_set(TH_Db* db, TH_Id id, TH_Flag flag, B32 value);

// Iterate the things carrying `flag`, same protocol as th_first/th_next
internal TH_Id th_first_flagged(TH_Db* db, TH_Flag flag);
internal TH_Id th_next_flagged(TH_Db* db, TH_Flag flag, TH_Id id);

// Variables: scalars
typedef U16 TH_Var;
enum {
  TH_Var_Nil,
  TH_Var_MovePts,
  TH_Var_COUNT
};

internal F32* th_var(TH_Db* db, TH_Id id, TH_Var var);
internal F32 th_var_get(TH_Db* db, TH_Id id, TH_Var var);
// Safer write than *th_var() = ...; no-op on a nil id
internal void th_var_set(TH_Db* db, TH_Id id, TH_Var var, F32 value);

// Integer scalars: ticks, amounts, encoded enumerations -- anything that must
// stay exact beyond F32's 2^24 integer ceiling
typedef U16 TH_IVar;
enum {
  TH_IVar_Nil,
  TH_IVar_X,
  TH_IVar_Y,
  TH_IVar_Sprite,
  TH_IVar_COUNT
};

internal I32* th_ivar(TH_Db* db, TH_Id id, TH_IVar ivar);
internal I32 th_ivar_get(TH_Db* db, TH_Id id, TH_IVar ivar);
// Safer write than *th_ivar() = ...; no-op on a nil id
internal void th_ivar_set(TH_Db* db, TH_Id id, TH_IVar ivar, I32 value);

// Refs: one target per thing per ref kind -- single references like owner,
// goal, next. Only thing -> target is stored; reverse lookups ("who refs X?")
// come out via analysis.
typedef U16 TH_Ref;
enum {
  TH_Ref_Nil,
  TH_Ref_Next,
  TH_Ref_Goal,
  TH_Ref_COUNT,
};

internal TH_Id th_ref_get(TH_Db* db, TH_Ref ref, TH_Id thing);
internal void th_ref_set(TH_Db* db, TH_Ref ref, TH_Id thing, TH_Id target);

// Many to many relations. Fungible amounts are edges to an archetype thing
// (value = amount); an item with state of its own is a full thing carried via
// a ref instead.
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

// Return value of an edge, returning fallback if the edge is missing
internal F32 th_edge_value(TH_Edges edges, TH_Id id, F32 fallback);

// Every outgoing edge of `source` under `rel`, pushed on `arena`
internal TH_Edges th_edges(Arena* arena, TH_Db* db, TH_Relation rel, TH_Id source);

// Relations are (logically) matrices: a set of value 0 is equivalent to a removal
internal void th_edge_set(TH_Db* db, TH_Relation rel, TH_Id source, TH_Id target, F32 value);

// Fields: per-position facts, the positional mirror of the per-thing families
// above -- one dense column per kind, indexed by tile position instead of
// thing slot. Tiles have no identity or lifecycle; a position is its own key.
// The world's actual size (set once by world generation, at most
// TH_WORLD_MAX_DIM per axis) bounds reads and writes: out-of-world positions
// resolve to the shared nil objects, read-only by convention.

internal void th_world_size_set(TH_Db* db, I32 width, I32 height);
internal V2I th_world_size(TH_Db* db);
internal B32 th_world_in_bounds(TH_Db* db, V2I pos);

// Field variables: scalars per position
typedef U16 TH_Field;
enum {
  TH_Field_Nil,
  TH_Field_COUNT
};

internal F32* th_field(TH_Db* db, V2I pos, TH_Field field);
internal F32 th_field_get(TH_Db* db, V2I pos, TH_Field field);
// Safer write than *th_field() = ...; no-op out of the world
internal void th_field_set(TH_Db* db, V2I pos, TH_Field field, F32 value);

// Integer field scalars: ids, masks, encoded enumerations
typedef U16 TH_IField;
enum {
  TH_IField_Nil,
  TH_IField_Terrain,   // terrain type id (see worldgen's terrain table)
  TH_IField_RiverMask, // feature connection masks, bit d = toward Dir4 d
  TH_IField_RoadMask,
  TH_IField_COUNT
};

internal I32* th_ifield(TH_Db* db, V2I pos, TH_IField ifield);
internal I32 th_ifield_get(TH_Db* db, V2I pos, TH_IField ifield);
// Safer write than *th_ifield() = ...; no-op out of the world
internal void th_ifield_set(TH_Db* db, V2I pos, TH_IField ifield, I32 value);

// Single-bit access into mask ifields; bits index 0..31. Set returns the old
// value (like th_flag_set) and no-ops out of the world.
internal B32 th_ifield_get_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit);
internal B32 th_ifield_set_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit, B32 value);

// Field refs: one thing per position per ref kind. Reads validate the target,
// so a despawned thing reads back as nil.
typedef U16 TH_FieldRef;
enum {
  TH_FieldRef_Nil,
  // Assigns the tile to be associated
  // with some group
  TH_FieldRef_Home,
  TH_FieldRef_COUNT
};

internal TH_Id th_field_ref_get(TH_Db* db, TH_FieldRef ref, V2I pos);
internal void th_field_ref_set(TH_Db* db, TH_FieldRef ref, V2I pos, TH_Id target);

// Field flags: booleans per position, implemented as bitsets
typedef U8 TH_FieldFlag;
enum {
  TH_FieldFlag_Nil,
  TH_FieldFlag_COUNT
};

internal B32 th_field_flag_get(TH_Db* db, V2I pos, TH_FieldFlag flag);
// Return the old value
internal B32 th_field_flag_set(TH_Db* db, V2I pos, TH_FieldFlag flag, B32 value);
