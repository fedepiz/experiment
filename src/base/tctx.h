#pragma once

#include "base/core.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Thread Context
//
// One explicit home for all per-thread state; the only thread-local variable
// in the codebase is a single pointer to it. Every thread -- main included --
// must equip itself before touching anything per-thread:
//
//   TCTX tctx;                    // lives on the thread's own stack
//   tctx_init_and_equip(&tctx);
//   ...
//   tctx_release();
//
// An unequipped thread trips an assert on first use rather than silently
// misbehaving.

#define TCTX_SCRATCH_COUNT 2

typedef struct TCTX TCTX;
struct TCTX {
  Arena* scratch_pool[TCTX_SCRATCH_COUNT];
};

internal void  tctx_init_and_equip(TCTX* tctx);
internal void  tctx_release(void);
internal TCTX* tctx_get(void);

////////////////////////////////
//~ fp: Scratch Arenas
//
// Per-thread transient arenas for functions that need temporary memory but
// take no allocator argument. Pass any arenas you are also writing results to
// as conflicts, so the scratch you get is guaranteed to be a different one --
// otherwise transient pushes and caller-visible results would interleave on
// the same arena, and releasing the scratch would rewind away the results.
//
// Two arenas per thread suffice as long as no function needs scratch while
// holding results on both -- push TCTX_SCRATCH_COUNT up if that ever happens.

internal ArenaTemp arena_get_scratch(Arena** conflicts, U64 conflict_count);
internal void      arena_release_scratch(ArenaTemp temp);
