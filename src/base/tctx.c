#include "base/core.h"
#include "base/arena.h"
#include "base/tctx.h"

////////////////////////////////
//~ fp: Thread Context

global _Thread_local TCTX* tctx_thread_local;

internal void tctx_init_and_equip(TCTX* tctx) {
  MemoryZeroStruct(tctx);
  for(U64 idx = 0; idx < TCTX_SCRATCH_COUNT; idx += 1) {
    tctx->scratch_pool[idx] = arena_alloc();
  }
  tctx_thread_local = tctx;
}

internal void tctx_release(void) {
  TCTX* tctx = tctx_get();
  for(U64 idx = 0; idx < TCTX_SCRATCH_COUNT; idx += 1) {
    arena_release(tctx->scratch_pool[idx]);
    tctx->scratch_pool[idx] = 0;
  }
  tctx_thread_local = 0;
}

internal TCTX* tctx_get(void) {
  // A null pointer here means that this thread did not call
  // tctx_init_and_equip.
  AssertAlways(tctx_thread_local != 0);
  return tctx_thread_local;
}

////////////////////////////////
//~ fp: Scratch Arenas

internal ArenaTemp arena_get_scratch(Arena** conflicts, U64 conflict_count) {
  TCTX* tctx = tctx_get();
  ArenaTemp result = {0};
  for(U64 idx = 0; idx < TCTX_SCRATCH_COUNT; idx += 1) {
    Arena* candidate = tctx->scratch_pool[idx];
    B32 is_conflict = 0;
    for(U64 c_idx = 0; c_idx < conflict_count; c_idx += 1) {
      if(candidate == conflicts[c_idx]) { is_conflict = 1; break; }
    }
    if(!is_conflict) {
      result = arena_temp_begin(candidate);
      break;
    }
  }
  // A null pointer here means that each scratch arena is a conflict. Make
  // TCTX_SCRATCH_COUNT larger.
  AssertAlways(result.arena != 0);
  return result;
}

internal void arena_release_scratch(ArenaTemp temp) {
  arena_temp_end(temp);
}
