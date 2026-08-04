#pragma once

#include "base/core.h"

typedef struct {
  F32 x;
  F32 y;
} V2;

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
