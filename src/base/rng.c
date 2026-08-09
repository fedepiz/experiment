#include "base/core.h"
#include "base/rng.h"

////////////////////////////////
//~ fp: Random

// An odd constant from the golden ratio. A step of this size reaches each U64.
#define RNG__GOLDEN64 0x9E3779B97F4A7C15ull

// The two multipliers that a hash of a position applies to its coordinates.
#define RNG__X_PRIME 374761393u
#define RNG__Y_PRIME 668265263u

////////////////////////////////
//~ fp: Mixers

internal U64 rng_mix_u64(U64 x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

internal U32 rng_mix_u32(U32 x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

////////////////////////////////
//~ fp: Hashes

internal U64 rng_hash_u64(U64 seed, U64 key) {
  return rng_mix_u64(key ^ rng_mix_u64(seed * RNG__GOLDEN64));
}

internal U32 rng_hash_2d(U64 seed, I32 x, I32 y) {
  U32 h = (U32)(seed ^ (seed >> 32));
  h += (U32)x * RNG__X_PRIME + (U32)y * RNG__Y_PRIME;
  return rng_mix_u32(h);
}

internal U32 rng_hash_2d_stream(U64 seed, I32 x, I32 y, U32 stream) {
  U64 h = seed ^ ((U64)(U32)x * RNG__X_PRIME) ^ ((U64)(U32)y * RNG__Y_PRIME) ^
          ((U64)stream * RNG__GOLDEN64);
  return (U32)rng_mix_u64(h);
}

internal F32 rng_hash01_2d(U64 seed, I32 x, I32 y) {
  return (F32)rng_hash_2d(seed, x, y) * (1.0f / 4294967296.0f);
}

////////////////////////////////
//~ fp: Streams

internal Rng rng_make(U64 seed) {
  Rng rng = {seed};
  return rng;
}

internal Rng rng_stream(U64 seed, U64 salt) {
  Rng rng = {rng_hash_u64(seed, salt)};
  return rng;
}

internal U64 rng_next_u64(Rng* rng) {
  rng->state += RNG__GOLDEN64;
  return rng_mix_u64(rng->state);
}

internal U32 rng_next_u32(Rng* rng) {
  return (U32)(rng_next_u64(rng) >> 32); // the high half mixes hardest
}

internal U32 rng_next_u32_below(Rng* rng, U32 bound) {
  if(bound == 0) { return 0; }
  // The method of Lemire. In a product of two 32-bit numbers, the high half is
  // the result and the low half is the part after the point. A modulo alone
  // would give the first 2^32 % bound values one more chance than the others.
  // A rejection of each low half below that limit removes the difference.
  U64 product = (U64)rng_next_u32(rng) * (U64)bound;
  if((U32)product < bound) {
    U32 threshold = (U32)(0u - bound) % bound; // 2^32 mod bound
    while((U32)product < threshold) {
      product = (U64)rng_next_u32(rng) * (U64)bound;
    }
  }
  return (U32)(product >> 32);
}

internal I32 rng_next_i32_range(Rng* rng, I32 min, I32 max) {
  if(max <= min) { return min; }
  U64 span = (U64)((I64)max - (I64)min) + 1;
  // The full range of an I32 is too large for a bound, and it needs no bound.
  if(span > 0xFFFFFFFFull) { return (I32)((I64)min + (I64)rng_next_u32(rng)); }
  return (I32)((I64)min + (I64)rng_next_u32_below(rng, (U32)span));
}

internal F32 rng_next_f32(Rng* rng) {
  // 24 bits, which is the mantissa of an F32. No value therefore moves to a
  // value near it.
  return (F32)(rng_next_u64(rng) >> 40) * (1.0f / 16777216.0f);
}

internal F32 rng_next_f32_range(Rng* rng, F32 min, F32 max) {
  return min + (max - min) * rng_next_f32(rng);
}

internal B32 rng_chance(Rng* rng, F32 p) {
  return rng_next_f32(rng) < p; // [0,1) draw: p <= 0 never, p >= 1 always
}
