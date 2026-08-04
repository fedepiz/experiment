#include "base/core.h"
#include "base/math.h"

#include <math.h>

internal F32 f32_exp2(F32 x) {
  return exp2f(x);
}

internal B32 v2_eq(V2 a, V2 b) {
  return a.x == b.x && a.y == b.y;
}

internal V2 v2_add(V2 a, V2 b) {
  return (V2){a.x + b.x, a.y + b.y};
}

internal V2 v2_sub(V2 a, V2 b) {
  return (V2){a.x - b.x, a.y - b.y};
}

internal V2 v2_scale(V2 a, F32 c) {
  return (V2){a.x * c, a.y * c};
}

internal V2 v2_scaled_add(V2 a, V2 b, F32 c) {
  return (V2){a.x + b.x * c, a.y + b.y * c};
}

internal B32 v2i_eq(V2I a, V2I b) {
  return a.x == b.x && a.y == b.y;
}

internal V2I v2i_add(V2I a, V2I b) {
  return (V2I){a.x + b.x, a.y + b.y};
}

internal V2I v2i_sub(V2I a, V2I b) {
  return (V2I){a.x - b.x, a.y - b.y};
}

internal F32 v2_magnitude(V2 a) {
  return sqrtf(a.x * a.x + a.y * a.y);
}

internal V2 v2_norm(V2 a, V2 fallback) {
  F32 mag = v2_magnitude(a);
  if(mag == 0) { return fallback; }
  return (V2){a.x / mag, a.y / mag};
}
