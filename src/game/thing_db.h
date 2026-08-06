#pragma once
#include "base/core.h"
#include "base/strings.h"
#include "base/arena.h"

// Max number of things that can possibly exist
#define TH_NUM_THINGS 65000

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
