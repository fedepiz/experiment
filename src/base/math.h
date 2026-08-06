#pragma once

#include "base/core.h"

// 2^x -- the natural base for framerate-independent exponential rates: the
// factor per second is exp2(rate), so `rate` reads as doublings per second.
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

// Integer companion to V2, for grid/tile coordinates.
typedef struct {
  I32 x;
  I32 y;
} V2I;

internal B32 v2i_eq(V2I a, V2I b);
internal V2I v2i_add(V2I a, V2I b);
internal V2I v2i_sub(V2I a, V2I b);

typedef struct {
  F32 x;
  F32 y;
  F32 z;
  F32 w;
} V4;

internal V4 v4_splat(F32 v);

// min is the top-left corner, max the bottom-right (y grows downward in
// window space).
typedef struct {
  V2 min;
  V2 max;
} Rect;

// Index order for per-corner data (colors, radii, ...).
typedef U32 Corner;
enum {
  Corner_TL,
  Corner_TR,
  Corner_BL,
  Corner_BR,
  Corner_COUNT,
};
