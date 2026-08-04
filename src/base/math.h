#pragma once

#include "base/core.h"

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

typedef struct {
  F32 x;
  F32 y;
  F32 z;
  F32 w;
} V4;

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
