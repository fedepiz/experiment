#include "base/core.h"
#include "base/os.h"
#include "base/arena.h"

////////////////////////////////
//~ fp: Arena Functions

internal Arena* arena_alloc_sized(U64 res_size, U64 cmt_size) {
  U64 page_size = os_page_size();
  res_size = AlignPow2(res_size, page_size);
  cmt_size = AlignPow2(cmt_size, page_size);
  cmt_size = ClampTop(cmt_size, res_size);
  Assert(cmt_size >= ARENA_HEADER_SIZE);

  Arena* arena = 0;
  void* base = os_reserve(res_size);
  if(base != 0 && os_commit(base, cmt_size)) {
    arena = (Arena*)base;
    arena->res = res_size;
    arena->cmt = cmt_size;
    arena->pos = ARENA_HEADER_SIZE;
  }
  AssertAlways(arena != 0);
  return arena;
}

internal Arena* arena_alloc(void) {
  return arena_alloc_sized(ARENA_DEFAULT_RESERVE_SIZE, ARENA_DEFAULT_COMMIT_SIZE);
}

internal void arena_release(Arena* arena) {
  os_release(arena, arena->res);
}

internal void* arena_push(Arena* arena, U64 size, U64 align) {
  Assert(IsPow2(align));
  U64 pos_pre = AlignPow2(arena->pos, align);
  U64 pos_post = pos_pre + size;

  // extend the committed range if this push runs past it
  if(arena->cmt < pos_post && pos_post <= arena->res) {
    U64 cmt_post = AlignPow2(pos_post, ARENA_COMMIT_GRANULARITY);
    cmt_post = ClampTop(cmt_post, arena->res);
    if(os_commit((U8*)arena + arena->cmt, cmt_post - arena->cmt)) {
      arena->cmt = cmt_post;
    }
  }

  void* result = 0;
  if(pos_post <= arena->cmt) {
    result = (U8*)arena + pos_pre;
    arena->pos = pos_post;
  }

  // null here means the reservation was exhausted -- grow the arena's reserve
  // size, or split the work across arenas
  AssertAlways(result != 0);
  return result;
}

internal U64 arena_pos(Arena* arena) {
  return arena->pos;
}

internal void arena_pop_to(Arena* arena, U64 pos) {
  U64 new_pos = ClampBot(pos, ARENA_HEADER_SIZE);
  Assert(new_pos <= arena->pos);
  arena->pos = new_pos;
}

internal void arena_clear(Arena* arena) {
  arena_pop_to(arena, ARENA_HEADER_SIZE);
}

////////////////////////////////
//~ fp: Temporary Arena Scopes

internal ArenaTemp arena_temp_begin(Arena* arena) {
  ArenaTemp temp = {arena, arena->pos};
  return temp;
}

internal void arena_temp_end(ArenaTemp temp) {
  arena_pop_to(temp.arena, temp.pos);
}
