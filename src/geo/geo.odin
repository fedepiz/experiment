package geo

import "core:math"

////////////////////////////////
//~ fp: Vectors
//
// A vector is a fixed array, so the arithmetic of the components — a + b,
// a * c, a == b — belongs to the language. Only the operations with a meaning
// beyond one component live here.

V2 :: [2]f32
V2i :: [2]int
V4 :: [4]f32

magnitude :: proc(a: V2) -> f32 {
	return math.sqrt(a.x * a.x + a.y * a.y)
}

// The zero vector has no direction, so it gives `fallback`.
norm :: proc(a: V2, fallback: V2) -> V2 {
	mag := magnitude(a)
	if mag == 0 {return fallback}
	return a / mag
}

////////////////////////////////
//~ fp: Directions

// The four neighbours of a cell, in order from the north, in the direction of
// a clock. Each part of the project that joins four cells on a V2i grid uses
// these names. The order is necessary: the opposite direction is +2, modulo 4.
Dir4 :: enum {
	N,
	E,
	S,
	W,
}

@(private)
dir4_deltas := [Dir4]V2i {
	.N = {0, -1},
	.E = {1, 0},
	.S = {0, 1},
	.W = {-1, 0},
}

dir4_delta :: proc(dir: Dir4) -> V2i {
	return dir4_deltas[dir]
}

dir4_opposite :: proc(dir: Dir4) -> Dir4 {
	return Dir4((int(dir) + 2) % len(Dir4))
}

// The direction of a single step. ok is false for each other delta.
dir4_from_delta :: proc(delta: V2i) -> (dir: Dir4, ok: bool) {
	for d in Dir4 {
		if dir4_deltas[d] == delta {return d, true}
	}
	return .N, false
}

////////////////////////////////
//~ fp: Ranges

// A rectangle of cells on a V2i grid. Both corners are inside the range. A
// range whose max is below its min on an axis is empty, and holds no cell.
Rng2i :: struct {
	min: V2i,
	max: V2i,
}

rng2i_is_empty :: proc(rng: Rng2i) -> bool {
	return rng.max.x < rng.min.x || rng.max.y < rng.min.y
}

rng2i_contains :: proc(rng: Rng2i, p: V2i) -> bool {
	return rng.min.x <= p.x && p.x <= rng.max.x && rng.min.y <= p.y && p.y <= rng.max.y
}

// empty when a and b share no cell
rng2i_intersect :: proc(a, b: Rng2i) -> Rng2i {
	return {
		{max(a.min.x, b.min.x), max(a.min.y, b.min.y)},
		{min(a.max.x, b.max.x), min(a.max.y, b.max.y)},
	}
}

// the number of cells, which is 0 for an empty range
rng2i_area :: proc(rng: Rng2i) -> int {
	if rng2i_is_empty(rng) {return 0}
	return (rng.max.x - rng.min.x + 1) * (rng.max.y - rng.min.y + 1)
}

////////////////////////////////
//~ fp: Rectangles

// `min` is the top left corner, and `max` is the bottom right corner. In the
// space of a window, y increases downward.
Rect :: struct {
	min: V2,
	max: V2,
}

// The order of the indices for data at each corner, such as a color or a
// radius.
Corner :: enum {
	TL,
	TR,
	BL,
	BR,
}
