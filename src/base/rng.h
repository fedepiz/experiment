#pragma once

#include "base/core.h"

////////////////////////////////
//~ fp: Random
//
// Two ways to draw numbers, over one set of mixers.
//
// Hashes are pure functions of their inputs: one (seed, key) yields one
// value, in any call order, however many times. Position-keyed noise and
// order-independent tie-breaks want this -- there is no state to thread and
// no sequence to keep in step.
//
// Streams carry a state that advances per draw, for sequences where the
// caller has no natural key to hash. A stream is a mixer walking its own
// input, so both halves draw from the same quality. Nothing here is
// cryptographic.

////////////////////////////////
//~ fp: Mixers
//
// Avalanche finalizers: flipping one input bit flips about half the output
// bits. Everything below is built on these.

internal U64 rng_mix_u64(U64 x); // splitmix64's finalizer
internal U32 rng_mix_u32(U32 x);

////////////////////////////////
//~ fp: Hashes
//
// Stateless keyed draws. `seed` separates whole worlds; `stream` separates
// the several independent decisions one caller makes at a single position.

internal U64 rng_hash_u64(U64 seed, U64 key);
internal U32 rng_hash_2d(U64 seed, I32 x, I32 y);
internal U32 rng_hash_2d_stream(U64 seed, I32 x, I32 y, U32 stream);
internal F32 rng_hash01_2d(U64 seed, I32 x, I32 y); // [0, 1)

////////////////////////////////
//~ fp: Streams
//
// Draws from one advancing state. Every seed is valid, 0 included.

typedef struct {
  U64 state;
} Rng;

internal Rng rng_make(U64 seed);
// a substream of `seed`; distinct salts never share a sequence
internal Rng rng_stream(U64 seed, U64 salt);

internal U64 rng_next_u64(Rng* rng);
internal U32 rng_next_u32(Rng* rng);

// [0, bound), every value equally likely; 0 when bound is 0
internal U32 rng_next_u32_below(Rng* rng, U32 bound);
// [min, max], both ends included; min when max <= min
internal I32 rng_next_i32_range(Rng* rng, I32 min, I32 max);

internal F32 rng_next_f32(Rng* rng);                         // [0, 1)
internal F32 rng_next_f32_range(Rng* rng, F32 min, F32 max); // [min, max)

// true with probability `p`, drawing exactly once whatever `p` is
internal B32 rng_chance(Rng* rng, F32 p);
