#pragma once

#include "base/core.h"

////////////////////////////////
//~ fp: Arena
//
// Linear bump allocator over one big virtual reservation. The arena header
// lives at the base of its own reservation, so an Arena* is also the base
// address of the memory it manages. Pages are committed lazily as the position
// advances; freeing is rewinding the position, not releasing individual
// allocations.

#define ARENA_HEADER_SIZE          64
#define ARENA_DEFAULT_RESERVE_SIZE GB(1)
#define ARENA_DEFAULT_COMMIT_SIZE  KB(64)
#define ARENA_COMMIT_GRANULARITY   KB(64)

typedef struct Arena Arena;
struct Arena {
  U64 res; // reserved bytes, including this header
  U64 cmt; // committed bytes
  U64 pos; // bump position, as an offset from the arena base
};
StaticAssert(sizeof(Arena) <= ARENA_HEADER_SIZE, arena_header_fits);

// A position saved at scope entry; arena_temp_end rewinds to it, reclaiming
// everything pushed inside the scope at zero cost.
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
