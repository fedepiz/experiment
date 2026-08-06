#include "base/core.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/thing_db.h"

////////////////////////////////
//~ fp: Thing Database
//
// One contiguous struct of fixed-size tables -- dense rows indexed by slot for
// facts, index-linked pools for the sparse parts (words, edges). No pointers
// anywhere inside, so the whole TH_Db is relocatable and saves as one blob.

#define TH_BITSET_WORDS    ((TH_NUM_THINGS + 63) / 64)
#define TH_MAX_WORDS       8192
#define TH_WORD_HASH_SLOTS 16384 // power of two, > 2x TH_MAX_WORDS: never fills
#define TH_WORD_CHARS      KB(256)
#define TH_MAX_EDGES       (1 << 18)

StaticAssert(IsPow2(TH_WORD_HASH_SLOTS), th_word_hash_pow2);
StaticAssert(TH_WORD_HASH_SLOTS >= 2 * TH_MAX_WORDS, th_word_hash_room);

typedef struct {
  U16 rel; // TH_Relation_Nil marks a free entry
  U16 source;
  U16 target;
  F32 value;
  U32 next; // next edge of the same (rel, source); freelist link while free
} TH__Edge;

struct TH_Db {
  //- fp: things. alive & ~nascent is what iteration sees; doomed things stay
  // alive until th_commit destroys them.
  U64 alive[TH_BITSET_WORDS];
  U64 nascent[TH_BITSET_WORDS];
  U64 doomed[TH_BITSET_WORDS];
  U16 generation[TH_NUM_THINGS];
  U16 free_next[TH_NUM_THINGS]; // freelist links through despawned slots
  U16 first_free;               // 0 = empty; slot 0 is never on the list
  U16 watermark;                // first never-used slot
  U16 doomed_count;             // marks since the last commit

  //- fp: words. Interned once, never freed; word 0 is nil.
  U32 word_offset[TH_MAX_WORDS];
  U8 word_len[TH_MAX_WORDS];
  U16 word_hash[TH_WORD_HASH_SLOTS]; // open-addressed word ids; 0 = empty
  U8 word_chars[TH_WORD_CHARS];
  U32 word_count;
  U32 word_chars_used;

  //- fp: facts, dense: table[kind][slot]. Kind 0 rows sit unused so kinds
  // index directly; untouched rows cost only zero pages.
  TH_Phrase labels[TH_Label_COUNT][TH_NUM_THINGS];
  U64 flags[TH_Flag_COUNT][TH_BITSET_WORDS];
  F32 vars[TH_Var_COUNT][TH_NUM_THINGS];
  I32 ivars[TH_IVar_COUNT][TH_NUM_THINGS];
  TH_Id refs[TH_Ref_COUNT][TH_NUM_THINGS];

  //- fp: edges, pooled: per-(rel, source) lists threaded by index. Edge 0 is
  // the nil entry, so 0 terminates lists.
  TH__Edge edges[TH_MAX_EDGES];
  U32 edge_first[TH_Relation_COUNT][TH_NUM_THINGS];
  U32 edge_free;
  U32 edge_watermark;

  //- fp: fields, dense: table[kind][cell], cell = y * TH_WORLD_MAX_DIM + x.
  // Columns are fixed at TH_WORLD_CELLS; world_width/height bound the part in
  // use. Kind 0 rows sit unused, as with the per-thing tables.
  I32 world_width;
  I32 world_height;
  F32 fields[TH_Field_COUNT][TH_WORLD_CELLS];
  I32 ifields[TH_IField_COUNT][TH_WORLD_CELLS];
  TH_Id field_refs[TH_FieldRef_COUNT][TH_WORLD_CELLS];
  U64 field_flags[TH_FieldFlag_COUNT][TH_WORLD_CELLS / 64];
};

////////////////////////////////
//~ fp: Nil

global TH_Phrase TH_NIL_PHRASE;
global F32 TH_NIL_VAR;
global I32 TH_NIL_IVAR;

////////////////////////////////
//~ fp: Internals

internal B32 th__bit_get(U64* bits, U32 idx) {
  return (bits[idx >> 6] >> (idx & 63)) & 1;
}

internal void th__bit_set(U64* bits, U32 idx, B32 value) {
  U64 mask = (U64)1 << (idx & 63);
  if(value) {
    bits[idx >> 6] |= mask;
  } else {
    bits[idx >> 6] &= ~mask;
  }
}

// ids pack as (slot << 16) | generation, so numeric order is slot-major

// live slot for id -- alive now, generation matches; 0 for nil / stale
internal U32 th__slot(TH_Db* db, TH_Id id) {
  U32 slot = id >> 16;
  if(slot == 0 || slot >= TH_NUM_THINGS) { return 0; }
  if(!th__bit_get(db->alive, slot)) { return 0; }
  if(db->generation[slot] != (U16)(id & 0xFFFF)) { return 0; }
  return slot;
}

internal TH_Id th__id_from_slot(TH_Db* db, U32 slot) {
  return (slot << 16) | db->generation[slot];
}

// first visible (alive & ~nascent) slot at or after start; nil when none
internal TH_Id th__scan_from(TH_Db* db, U32 start) {
  for(U32 slot = start; slot < db->watermark;) {
    U64 word = (db->alive[slot >> 6] & ~db->nascent[slot >> 6]) >> (slot & 63);
    if(word == 0) {
      slot = AlignDownPow2(slot, 64) + 64;
      continue;
    }
    if(word & 1) { return th__id_from_slot(db, slot); }
    slot += 1;
  }
  return 0;
}

////////////////////////////////
//~ fp: Lifetime

internal TH_Db* th_init_db(Arena* arena) {
  TH_Db* db = push_array(arena, TH_Db, 1);
  db->watermark = 1;      // slot 0 is the nil thing
  db->word_count = 1;     // word 0 is the nil word
  db->edge_watermark = 1; // edge 0 is the nil entry
  return db;
}

internal TH_Id th_spawn(TH_Db* db) {
  U32 slot = db->first_free;
  if(slot != 0) {
    db->first_free = db->free_next[slot];
  } else if(db->watermark < TH_NUM_THINGS) {
    slot = db->watermark;
    db->watermark += 1;
  } else {
    return 0;
  }
  th__bit_set(db->alive, slot, 1);
  th__bit_set(db->nascent, slot, 1);
  return th__id_from_slot(db, slot);
}

internal void th_despawn_mark(TH_Db* db, TH_Id id) {
  U32 slot = th__slot(db, id);
  if(slot != 0 && !th__bit_get(db->doomed, slot)) {
    th__bit_set(db->doomed, slot, 1);
    db->doomed_count += 1;
  }
}

internal void th_commit(TH_Db* db) {
  if(db->doomed_count > 0) {
    //- fp: rebuild edge lists from the pool, dropping every edge that touches
    // a doomed thing -- the batched sweep that lets edges store one direction
    MemoryZero(db->edge_first, sizeof(db->edge_first));
    db->edge_free = 0;
    for(U32 e = 1; e < db->edge_watermark; e += 1) {
      TH__Edge* edge = &db->edges[e];
      B32 dead = edge->rel == TH_Relation_Nil || th__bit_get(db->doomed, edge->source) || th__bit_get(db->doomed, edge->target);
      if(dead) {
        edge->rel = TH_Relation_Nil;
        edge->next = db->edge_free;
        db->edge_free = e;
      } else {
        edge->next = db->edge_first[edge->rel][edge->source];
        db->edge_first[edge->rel][edge->source] = e;
      }
    }
    //- fp: destroy doomed things: zero every fact row, stale the id, recycle
    for(U32 word_idx = 0; word_idx < TH_BITSET_WORDS; word_idx += 1) {
      for(U64 word = db->doomed[word_idx]; word != 0;) {
        U32 bit = 0;
        for(; !(word & ((U64)1 << bit)); bit += 1) {}
        word &= ~((U64)1 << bit);
        U32 slot = word_idx * 64 + bit;
        for(U32 label = 1; label < TH_Label_COUNT; label += 1) { MemoryZeroStruct(&db->labels[label][slot]); }
        for(U32 flag = 1; flag < TH_Flag_COUNT; flag += 1) { th__bit_set(db->flags[flag], slot, 0); }
        for(U32 var = 1; var < TH_Var_COUNT; var += 1) { db->vars[var][slot] = 0; }
        for(U32 ivar = 1; ivar < TH_IVar_COUNT; ivar += 1) { db->ivars[ivar][slot] = 0; }
        for(U32 ref = 1; ref < TH_Ref_COUNT; ref += 1) { db->refs[ref][slot] = 0; }
        db->generation[slot] += 1;
        th__bit_set(db->alive, slot, 0);
        db->free_next[slot] = db->first_free;
        db->first_free = (U16)slot;
      }
    }
    MemoryZeroArray(db->doomed);
    db->doomed_count = 0;
  }
  //- fp: spawns become visible
  MemoryZeroArray(db->nascent);
}

////////////////////////////////
//~ fp: Iteration

internal TH_Id th_first(TH_Db* db) {
  return th__scan_from(db, 1);
}

internal TH_Id th_next(TH_Db* db, TH_Id id) {
  if(id == 0) { return 0; }
  return th__scan_from(db, (id >> 16) + 1);
}

internal TH_Id th_first_flagged(TH_Db* db, TH_Flag flag) {
  TH_Id id = th_first(db);
  for(; id != 0 && !th_flag_get(db, id, flag); id = th_next(db, id)) {}
  return id;
}

internal TH_Id th_next_flagged(TH_Db* db, TH_Flag flag, TH_Id id) {
  id = th_next(db, id);
  for(; id != 0 && !th_flag_get(db, id, flag); id = th_next(db, id)) {}
  return id;
}

////////////////////////////////
//~ fp: Words

internal String8 th__word_str(TH_Db* db, TH_Word word) {
  return str8(db->word_chars + db->word_offset[word], db->word_len[word]);
}

internal U64 th__hash_str8(String8 s) {
  U64 hash = 0xcbf29ce484222325ull; // fnv-1a
  for(U64 idx = 0; idx < s.size; idx += 1) {
    hash = (hash ^ s.str[idx]) * 0x100000001b3ull;
  }
  return hash;
}

internal TH_Word th_define_word(TH_Db* db, String8 source) {
  if(source.size == 0 || source.size > 255) { return 0; }
  U32 mask = TH_WORD_HASH_SLOTS - 1;
  for(U32 probe = (U32)th__hash_str8(source) & mask;; probe = (probe + 1) & mask) {
    TH_Word word = db->word_hash[probe];
    if(word == 0) {
      if(db->word_count >= TH_MAX_WORDS) { return 0; }
      if(db->word_chars_used + source.size > TH_WORD_CHARS) { return 0; }
      word = (TH_Word)db->word_count;
      db->word_count += 1;
      db->word_offset[word] = db->word_chars_used;
      db->word_len[word] = (U8)source.size;
      MemoryCopy(db->word_chars + db->word_chars_used, source.str, source.size);
      db->word_chars_used += (U32)source.size;
      db->word_hash[probe] = word;
      return word;
    }
    if(str8_match(th__word_str(db, word), source, 0)) { return word; }
  }
}

internal String8 th_resolve_word(TH_Db* db, TH_Word word, String8 fallback) {
  if(word == 0 || word >= db->word_count) { return fallback; }
  return th__word_str(db, word);
}

internal B32 th_push_word(TH_Phrase* phrase, TH_Word word) {
  if(phrase->len >= TH_PHRASE_MAX_LEN) { return false; }
  phrase->words[phrase->len] = word;
  phrase->len += 1;
  return true;
}

internal String8 th_resolve_phrase(Arena* out, TH_Db* db, TH_Phrase phrase, String8 fallback) {
  ArenaTemp scratch = arena_get_scratch(&out, 1);
  String8List list = {0};
  U8 len = ClampTop(phrase.len, TH_PHRASE_MAX_LEN);
  for(U8 idx = 0; idx < len; idx += 1) {
    String8 word = th_resolve_word(db, phrase.words[idx], str8_lit(""));
    if(word.size > 0) { str8_list_push(scratch.arena, &list, word); }
  }
  String8 result = fallback;
  if(list.node_count > 0) {
    StringJoin join = {.sep = str8_lit(" ")};
    result = str8_list_join(out, list, &join);
  }
  arena_release_scratch(scratch);
  return result;
}

////////////////////////////////
//~ fp: Labels

internal TH_Phrase* th_label(TH_Db* db, TH_Id id, TH_Label label) {
  U32 slot = th__slot(db, id);
  if(slot == 0 || label == TH_Label_Nil || label >= TH_Label_COUNT) { return &TH_NIL_PHRASE; }
  return &db->labels[label][slot];
}

////////////////////////////////
//~ fp: Flags

internal B32 th_flag_get(TH_Db* db, TH_Id id, TH_Flag flag) {
  U32 slot = th__slot(db, id);
  if(slot == 0 || flag == TH_Flag_Nil || flag >= TH_Flag_COUNT) { return false; }
  return th__bit_get(db->flags[flag], slot);
}

internal B32 th_flag_set(TH_Db* db, TH_Id id, TH_Flag flag, B32 value) {
  U32 slot = th__slot(db, id);
  if(slot == 0 || flag == TH_Flag_Nil || flag >= TH_Flag_COUNT) { return false; }
  B32 old = th__bit_get(db->flags[flag], slot);
  th__bit_set(db->flags[flag], slot, value);
  return old;
}

////////////////////////////////
//~ fp: Variables

internal F32* th_var(TH_Db* db, TH_Id id, TH_Var var) {
  U32 slot = th__slot(db, id);
  if(slot == 0 || var == TH_Var_Nil || var >= TH_Var_COUNT) { return &TH_NIL_VAR; }
  return &db->vars[var][slot];
}

internal F32 th_var_get(TH_Db* db, TH_Id id, TH_Var var) {
  return *th_var(db, id, var);
}

internal void th_var_set(TH_Db* db, TH_Id id, TH_Var var, F32 value) {
  F32* p = th_var(db, id, var);
  if(p != &TH_NIL_VAR) { *p = value; }
}

internal I32* th_ivar(TH_Db* db, TH_Id id, TH_IVar ivar) {
  U32 slot = th__slot(db, id);
  if(slot == 0 || ivar == TH_IVar_Nil || ivar >= TH_IVar_COUNT) { return &TH_NIL_IVAR; }
  return &db->ivars[ivar][slot];
}

internal I32 th_ivar_get(TH_Db* db, TH_Id id, TH_IVar ivar) {
  return *th_ivar(db, id, ivar);
}

internal void th_ivar_set(TH_Db* db, TH_Id id, TH_IVar ivar, I32 value) {
  I32* p = th_ivar(db, id, ivar);
  if(p != &TH_NIL_IVAR) { *p = value; }
}

////////////////////////////////
//~ fp: Refs

internal TH_Id th_ref_get(TH_Db* db, TH_Ref ref, TH_Id thing) {
  U32 slot = th__slot(db, thing);
  if(slot == 0 || ref == TH_Ref_Nil || ref >= TH_Ref_COUNT) { return 0; }
  TH_Id target = db->refs[ref][slot];
  if(th__slot(db, target) == 0) { return 0; } // target despawned since
  return target;
}

internal void th_ref_set(TH_Db* db, TH_Ref ref, TH_Id thing, TH_Id target) {
  U32 slot = th__slot(db, thing);
  if(slot == 0 || ref == TH_Ref_Nil || ref >= TH_Ref_COUNT) { return; }
  if(th__slot(db, target) == 0) { target = 0; } // nil / stale clears
  db->refs[ref][slot] = target;
}

////////////////////////////////
//~ fp: Relations

internal F32 th_edge_value(TH_Edges edges, TH_Id id, F32 fallback) {
  for(U64 idx = 0; idx < edges.count; idx += 1) {
    if(edges.entries[idx].id == id) { return edges.entries[idx].value; }
  }
  return fallback;
}

internal TH_Edges th_edges(Arena* arena, TH_Db* db, TH_Relation rel, TH_Id source) {
  TH_Edges result = {0};
  U32 slot = th__slot(db, source);
  if(slot == 0 || rel == TH_Relation_Nil || rel >= TH_Relation_COUNT) { return result; }
  U64 count = 0;
  for(U32 e = db->edge_first[rel][slot]; e != 0; e = db->edges[e].next) { count += 1; }
  result.entries = push_array_no_zero(arena, TH_EdgeEntry, count);
  for(U32 e = db->edge_first[rel][slot]; e != 0; e = db->edges[e].next) {
    TH__Edge* edge = &db->edges[e];
    result.entries[result.count] = (TH_EdgeEntry){
        .id = th__id_from_slot(db, edge->target),
        .value = edge->value,
    };
    result.count += 1;
  }
  return result;
}

internal void th_edge_set(TH_Db* db, TH_Relation rel, TH_Id source, TH_Id target, F32 value) {
  U32 src = th__slot(db, source);
  U32 tgt = th__slot(db, target);
  if(src == 0 || tgt == 0 || rel == TH_Relation_Nil || rel >= TH_Relation_COUNT) { return; }
  //- fp: existing edge: update, or unlink when set to 0
  for(U32* link = &db->edge_first[rel][src]; *link != 0; link = &db->edges[*link].next) {
    TH__Edge* edge = &db->edges[*link];
    if(edge->target == tgt) {
      if(value == 0) {
        U32 e = *link;
        *link = edge->next;
        edge->rel = TH_Relation_Nil;
        edge->next = db->edge_free;
        db->edge_free = e;
      } else {
        edge->value = value;
      }
      return;
    }
  }
  if(value == 0) { return; }
  //- fp: new edge; exhaustion is a capacity tuning failure, so it trips loud
  U32 e = db->edge_free;
  if(e != 0) {
    db->edge_free = db->edges[e].next;
  } else if(db->edge_watermark < TH_MAX_EDGES) {
    e = db->edge_watermark;
    db->edge_watermark += 1;
  } else {
    Assert(!"edge pool exhausted");
    return;
  }
  db->edges[e] = (TH__Edge){
      .rel = rel,
      .source = (U16)src,
      .target = (U16)tgt,
      .value = value,
      .next = db->edge_first[rel][src],
  };
  db->edge_first[rel][src] = e;
}

////////////////////////////////
//~ fp: Fields

internal void th_world_size_set(TH_Db* db, I32 width, I32 height) {
  Assert(1 <= width && width <= TH_WORLD_MAX_DIM);
  Assert(1 <= height && height <= TH_WORLD_MAX_DIM);
  db->world_width = width;
  db->world_height = height;
}

internal V2I th_world_size(TH_Db* db) {
  return (V2I){db->world_width, db->world_height};
}

internal B32 th_world_in_bounds(TH_Db* db, V2I pos) {
  return 0 <= pos.x && pos.x < db->world_width &&
         0 <= pos.y && pos.y < db->world_height;
}

internal U32 th__cell(V2I pos) {
  return (U32)pos.y * TH_WORLD_MAX_DIM + (U32)pos.x;
}

internal F32* th_field(TH_Db* db, V2I pos, TH_Field field) {
  if(!th_world_in_bounds(db, pos) || field == TH_Field_Nil || field >= TH_Field_COUNT) { return &TH_NIL_VAR; }
  return &db->fields[field][th__cell(pos)];
}

internal F32 th_field_get(TH_Db* db, V2I pos, TH_Field field) {
  return *th_field(db, pos, field);
}

internal void th_field_set(TH_Db* db, V2I pos, TH_Field field, F32 value) {
  F32* p = th_field(db, pos, field);
  if(p != &TH_NIL_VAR) { *p = value; }
}

internal I32* th_ifield(TH_Db* db, V2I pos, TH_IField ifield) {
  if(!th_world_in_bounds(db, pos) || ifield == TH_IField_Nil || ifield >= TH_IField_COUNT) { return &TH_NIL_IVAR; }
  return &db->ifields[ifield][th__cell(pos)];
}

internal I32 th_ifield_get(TH_Db* db, V2I pos, TH_IField ifield) {
  return *th_ifield(db, pos, ifield);
}

internal void th_ifield_set(TH_Db* db, V2I pos, TH_IField ifield, I32 value) {
  I32* p = th_ifield(db, pos, ifield);
  if(p != &TH_NIL_IVAR) { *p = value; }
}

internal B32 th_ifield_get_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit) {
  if(bit >= 32) { return false; }
  return (th_ifield_get(db, pos, ifield) >> bit) & 1;
}

internal B32 th_ifield_set_bit(TH_Db* db, V2I pos, TH_IField ifield, U32 bit, B32 value) {
  I32* p = th_ifield(db, pos, ifield);
  if(p == &TH_NIL_IVAR || bit >= 32) { return false; }
  B32 old = (*p >> bit) & 1;
  if(value) {
    *p |= (I32)(1u << bit);
  } else {
    *p &= ~(I32)(1u << bit);
  }
  return old;
}

internal TH_Id th_field_ref_get(TH_Db* db, TH_FieldRef ref, V2I pos) {
  if(!th_world_in_bounds(db, pos) || ref == TH_FieldRef_Nil || ref >= TH_FieldRef_COUNT) { return 0; }
  TH_Id target = db->field_refs[ref][th__cell(pos)];
  if(th__slot(db, target) == 0) { return 0; } // target despawned since
  return target;
}

internal void th_field_ref_set(TH_Db* db, TH_FieldRef ref, V2I pos, TH_Id target) {
  if(!th_world_in_bounds(db, pos) || ref == TH_FieldRef_Nil || ref >= TH_FieldRef_COUNT) { return; }
  if(th__slot(db, target) == 0) { target = 0; } // nil / stale clears
  db->field_refs[ref][th__cell(pos)] = target;
}

internal B32 th_field_flag_get(TH_Db* db, V2I pos, TH_FieldFlag flag) {
  if(!th_world_in_bounds(db, pos) || flag == TH_FieldFlag_Nil || flag >= TH_FieldFlag_COUNT) { return false; }
  return th__bit_get(db->field_flags[flag], th__cell(pos));
}

internal B32 th_field_flag_set(TH_Db* db, V2I pos, TH_FieldFlag flag, B32 value) {
  if(!th_world_in_bounds(db, pos) || flag == TH_FieldFlag_Nil || flag >= TH_FieldFlag_COUNT) { return false; }
  U32 cell = th__cell(pos);
  B32 old = th__bit_get(db->field_flags[flag], cell);
  th__bit_set(db->field_flags[flag], cell, value);
  return old;
}
