#pragma once

#include "base/core.h"

////////////////////////////////
//~ fp: Random
//
// Two ways to take a number, over one set of mixers.
//
// A hash is a pure function of its inputs. One (seed, key) gives one value, in
// any order of the calls, and at each call. A noise from a position needs
// this, and a rank that does not depend on an order needs it too. There is no
// state to pass, and no sequence to keep equal.
//
// A stream holds a state that moves forward at each draw. Use a stream for a
// sequence where the caller has no natural key to hash. A stream is a mixer
// over its own output, so both ways give values of the same quality.
//
// Nothing here is safe for cryptography.

////////////////////////////////
//~ fp: Mixers
//
// A mixer changes about half of the output bits when one input bit changes.
// Each function below uses a mixer.

internal U64 rng_mix_u64(U64 x); // the final step of splitmix64
internal U32 rng_mix_u32(U32 x);

////////////////////////////////
//~ fp: Hashes
//
// A draw from a key, with no state. `seed` separates two worlds. `stream`
// separates the independent choices that one caller makes at one position.

internal U64 rng_hash_u64(U64 seed, U64 key);
internal U32 rng_hash_2d(U64 seed, I32 x, I32 y);
internal U32 rng_hash_2d_stream(U64 seed, I32 x, I32 y, U32 stream);
internal F32 rng_hash01_2d(U64 seed, I32 x, I32 y); // [0, 1)

////////////////////////////////
//~ fp: Streams
//
// A draw from one state that moves forward. Each seed is valid, and 0 is valid.

typedef struct {
  U64 state;
} Rng;

internal Rng rng_make(U64 seed);
// A stream inside `seed`. Two different salts never give the same sequence.
internal Rng rng_stream(U64 seed, U64 salt);

internal U64 rng_next_u64(Rng* rng);
internal U32 rng_next_u32(Rng* rng);

// [0, bound), where each value has the same probability. It gives 0 when
// `bound` is 0.
internal U32 rng_next_u32_below(Rng* rng, U32 bound);
// [min, max], with both ends. It gives `min` when max is min or less.
internal I32 rng_next_i32_range(Rng* rng, I32 min, I32 max);

internal F32 rng_next_f32(Rng* rng);                         // [0, 1)
internal F32 rng_next_f32_range(Rng* rng, F32 min, F32 max); // [min, max)

// true with the probability `p`. It takes one value at each call, at any `p`.
internal B32 rng_chance(Rng* rng, F32 p);
