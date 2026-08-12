package rng

import "core:testing"

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
//
// These functions must draw the same values forever, or each world changes.
// The tests at the end of this file hold golden values that guard the
// sequences.

// An odd constant from the golden ratio. A step of this size reaches each u64.
@(private) GOLDEN64 :: 0x9E3779B97F4A7C15

// The two multipliers that a hash of a position applies to its coordinates.
@(private) X_PRIME :: 374761393
@(private) Y_PRIME :: 668265263

////////////////////////////////
//~ fp: Mixers
//
// A mixer changes about half of the output bits when one input bit changes.
// Each function below uses a mixer.

// the final step of splitmix64
mix_u64 :: proc(x: u64) -> u64 {
	x := x
	x ~= x >> 30
	x *= 0xBF58476D1CE4E5B9
	x ~= x >> 27
	x *= 0x94D049BB133111EB
	x ~= x >> 31
	return x
}

mix_u32 :: proc(x: u32) -> u32 {
	x := x
	x ~= x >> 16
	x *= 0x7FEB352D
	x ~= x >> 15
	x *= 0x846CA68B
	x ~= x >> 16
	return x
}

////////////////////////////////
//~ fp: Hashes
//
// A draw from a key, with no state. `seed` separates two worlds. `stream`
// separates the independent choices that one caller makes at one position.

hash_u64 :: proc(seed, key: u64) -> u64 {
	return mix_u64(key ~ mix_u64(seed * GOLDEN64))
}

hash_2d :: proc(seed: u64, x, y: int) -> u32 {
	h := u32(seed ~ (seed >> 32))
	h += u32(x) * X_PRIME + u32(y) * Y_PRIME
	return mix_u32(h)
}

hash_2d_stream :: proc(seed: u64, x, y: int, stream: u32) -> u32 {
	h := seed ~ (u64(u32(x)) * X_PRIME) ~ (u64(u32(y)) * Y_PRIME) ~ (u64(stream) * GOLDEN64)
	return u32(mix_u64(h))
}

// [0, 1)
hash01_2d :: proc(seed: u64, x, y: int) -> f32 {
	return f32(hash_2d(seed, x, y)) * (1.0 / 4294967296.0)
}

////////////////////////////////
//~ fp: Streams
//
// A draw from one state that moves forward. Each seed is valid, and 0 is valid.

Stream :: struct {
	state: u64,
}

make :: proc(seed: u64) -> Stream {
	return {seed}
}

// A stream inside `seed`. Two different salts never give the same sequence.
stream :: proc(seed, salt: u64) -> Stream {
	return {hash_u64(seed, salt)}
}

next_u64 :: proc(r: ^Stream) -> u64 {
	r.state += GOLDEN64
	return mix_u64(r.state)
}

next_u32 :: proc(r: ^Stream) -> u32 {
	return u32(next_u64(r) >> 32) // the high half mixes hardest
}

// [0, bound), where each value has the same probability. It gives 0 when
// `bound` is 0.
next_u32_below :: proc(r: ^Stream, bound: u32) -> u32 {
	if bound == 0 { return 0 }
	// The method of Lemire. In a product of two 32-bit numbers, the high half is
	// the result and the low half is the part after the point. A modulo alone
	// would give the first 2^32 % bound values one more chance than the others.
	// A rejection of each low half below that limit removes the difference.
	product := u64(next_u32(r)) * u64(bound)
	if u32(product) < bound {
		threshold := (0 - bound) % bound // 2^32 mod bound
		for u32(product) < threshold {
			product = u64(next_u32(r)) * u64(bound)
		}
	}
	return u32(product >> 32)
}

// [min, max], with both ends. It gives `min` when max is min or less.
next_int_range :: proc(r: ^Stream, min, max: int) -> int {
	if max <= min { return min }
	span := u64(max - min) + 1
	// A span past the range of a u32 takes a whole draw, and needs no bound.
	if span > 0xFFFFFFFF { return min + int(next_u32(r)) }
	return min + int(next_u32_below(r, u32(span)))
}

// [0, 1)
next_f32 :: proc(r: ^Stream) -> f32 {
	// 24 bits, which is the mantissa of an f32. No value therefore moves to a
	// value near it.
	return f32(next_u64(r) >> 40) * (1.0 / 16777216.0)
}

// [min, max)
next_f32_range :: proc(r: ^Stream, min, max: f32) -> f32 {
	return min + (max - min) * next_f32(r)
}

// true with the probability `p`. It takes one value at each call, at any `p`.
chance :: proc(r: ^Stream, p: f32) -> bool {
	return next_f32(r) < p // [0,1) draw: p <= 0 never, p >= 1 always
}

////////////////////////////////
//~ fp: Tests

// The values below are golden: a change that moves one of them changes each
// world. The two f32 values compare as bits, so no decimal text sits between
// the test and the implementation.

@(test)
golden_values :: proc(t: ^testing.T) {
	testing.expect_value(t, mix_u64(0x123456789ABCDEF0), 10820449572363811078)
	testing.expect_value(t, mix_u32(0xDEADBEEF), 3861431939)
	testing.expect_value(t, hash_u64(2704, 42), 1633662610153915858)
	testing.expect_value(t, hash_2d(2704, -3, 7), 1335903638)
	testing.expect_value(t, hash_2d_stream(2704, -3, 7, 5), 834306763)
	testing.expect_value(t, transmute(u32)hash01_2d(2704, 11, -13), 1050028770)

	r := stream(2704, 99)
	testing.expect_value(t, next_u64(&r), 17741196471015734563)
	testing.expect_value(t, next_u32(&r), 2782395141)
	testing.expect_value(t, next_u32_below(&r, 10), 4)
	testing.expect_value(t, next_int_range(&r, -5, 17), 7)
	testing.expect_value(t, transmute(u32)next_f32(&r), 1061569986)
	testing.expect_value(t, chance(&r, 0.5), false)
	testing.expect_value(t, r.state, 13401580711423944531)

	z := make(0)
	testing.expect_value(t, next_u64(&z), 16294208416658607535)
}
