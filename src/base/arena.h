#pragma once

#include "base/core.h"

////////////////////////////////
//~ fp: Arena
//
// An arena gives out memory from one large reservation of addresses. Each
// allocation moves a position forward.
//
// The header of an arena is at the start of its own reservation, so an Arena*
// is also the first address of the memory that it manages.
//
// The arena commits a page when the position reaches it. To free memory, move
// the position back. The arena does not free one allocation.

#define ARENA_HEADER_SIZE          64
#define ARENA_DEFAULT_RESERVE_SIZE GB(1)
#define ARENA_DEFAULT_COMMIT_SIZE  KB(64)
#define ARENA_COMMIT_GRANULARITY   KB(64)

typedef struct Arena Arena;
struct Arena {
  U64 res; // the reserved bytes, with this header
  U64 cmt; // the committed bytes
  U64 pos; // the position, as an offset from the start of the arena
};
StaticAssert(sizeof(Arena) <= ARENA_HEADER_SIZE, arena_header_fits);

// A position that the caller saves at the start of a scope. arena_temp_end
// moves the arena back to it, and frees each allocation of that scope at no
// cost.
typedef struct {
  Arena* arena;
  U64 pos;
} ArenaTemp;

////////////////////////////////
//~ fp: Arena Functions

internal Arena* arena_alloc_sized(U64 res_size, U64 cmt_size);
internal Arena* arena_alloc(void);
internal void   arena_release(Arena* arena);

internal void* arena_push(Arena* arena, U64 size, U64 align);
internal U64   arena_pos(Arena* arena);
internal void  arena_pop_to(Arena* arena, U64 pos);
internal void  arena_clear(Arena* arena);

#define push_array_no_zero(a, T, c) (T*)arena_push((a), sizeof(T) * (c), AlignOf(T))
#define push_array(a, T, c)         (T*)MemoryZero(push_array_no_zero(a, T, c), sizeof(T) * (c))

internal ArenaTemp arena_temp_begin(Arena* arena);
internal void      arena_temp_end(ArenaTemp temp);
