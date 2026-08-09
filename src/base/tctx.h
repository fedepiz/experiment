#pragma once

#include "base/core.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Thread Context
//
// The thread context is the one place for the state of a thread. The only
// variable in this project with thread storage is one pointer to it.
//
// Each thread, and the main thread too, must set its context before it uses
// anything that belongs to a thread:
//
//   TCTX tctx;                    // on the stack of that thread
//   tctx_init_and_equip(&tctx);
//   ...
//   tctx_release();
//
// A thread with no context stops at an assert at its first use, and does not
// continue with a wrong result.

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
// A scratch arena is a temporary arena of one thread. Use one in a function
// that needs memory for a short time and that takes no arena as a parameter.
//
// Give each arena that you also write a result to as a conflict. You then get a
// different arena. If you do not, the temporary data and the result go on one
// arena, and the release of the scratch arena removes the result.
//
// Two arenas for each thread are enough, while no function needs a scratch
// arena and holds results on both of them. Make TCTX_SCRATCH_COUNT larger if
// that happens.

internal ArenaTemp arena_get_scratch(Arena** conflicts, U64 conflict_count);
internal void      arena_release_scratch(ArenaTemp temp);
