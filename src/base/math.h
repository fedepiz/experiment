#pragma once

#include "base/core.h"

// 2^x. Use this base for a rate that must not depend on the frame rate. The
// factor for one second is exp2(rate), so `rate` is the number of times that a
// value doubles in one second.
internal F32 f32_exp2(F32 x);

typedef struct {
  F32 x;
  F32 y;
} V2;

internal B32 v2_eq(V2 a, V2 b);
internal V2 v2_add(V2 a, V2 b);
internal V2 v2_sub(V2 a, V2 b);
internal V2 v2_scale(V2 a, F32 c);
internal V2 v2_scaled_add(V2 a, V2 b, F32 c);

internal F32 v2_magnitude(V2 a);
internal V2 v2_norm(V2 a, V2 fallback);

// The integer form of V2. Use it for the coordinates of a grid or a tile.
typedef struct {
  I32 x;
  I32 y;
} V2I;

internal B32 v2i_eq(V2I a, V2I b);
internal V2I v2i_add(V2I a, V2I b);
internal V2I v2i_sub(V2I a, V2I b);

// The four neighbours of a cell, in order from the north, in the direction of
// a clock. Each part of the project that joins four cells on a V2I grid uses
// these names. The order is necessary: the opposite direction is +2, modulo 4.
typedef U32 Dir4;
enum {
  Dir4_N,
  Dir4_E,
  Dir4_S,
  Dir4_W,
  Dir4_COUNT,
};

internal V2I  dir4_delta(Dir4 dir);
internal Dir4 dir4_opposite(Dir4 dir);
internal Dir4 dir4_from_delta(V2I delta); // Dir4_COUNT when delta isn't a single step

// A rectangle of cells on a V2I grid. Both corners are inside the range. A
// range whose max is below its min on an axis is empty, and holds no cell.
typedef struct {
  V2I min;
  V2I max;
} Rng2I32;

internal Rng2I32 rng2i32(V2I min, V2I max);
internal B32 rng2i32_is_empty(Rng2I32 rng);
internal B32 rng2i32_contains(Rng2I32 rng, V2I p);
internal Rng2I32 rng2i32_intersect(Rng2I32 a, Rng2I32 b); // empty when a and b share no cell
internal U64 rng2i32_area(Rng2I32 rng); // the number of cells, which is 0 for an empty range

typedef struct {
  F32 x;
  F32 y;
  F32 z;
  F32 w;
} V4;

internal V4 v4_splat(F32 v);

// `min` is the top left corner, and `max` is the bottom right corner. In the
// space of a window, y increases downward.
typedef struct {
  V2 min;
  V2 max;
} Rect;

// The order of the indices for data at each corner, such as a color or a
// radius.
typedef U32 Corner;
enum {
  Corner_TL,
  Corner_TR,
  Corner_BL,
  Corner_BR,
  Corner_COUNT,
};
