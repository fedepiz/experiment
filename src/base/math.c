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

global V2I dir4__deltas[Dir4_COUNT] = {
    {0, -1},
    {1, 0},
    {0, 1},
    {-1, 0},
};

internal V2I dir4_delta(Dir4 dir) {
  Assert(dir < Dir4_COUNT);
  return dir4__deltas[dir];
}

internal Dir4 dir4_opposite(Dir4 dir) {
  Assert(dir < Dir4_COUNT);
  return (dir + 2) % Dir4_COUNT;
}

internal Dir4 dir4_from_delta(V2I delta) {
  Dir4 result = Dir4_COUNT;
  for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
    if(v2i_eq(dir4__deltas[dir], delta)) {
      result = dir;
      break;
    }
  }
  return result;
}

internal Rng2I32 rng2i32(V2I min, V2I max) {
  return (Rng2I32){min, max};
}

internal B32 rng2i32_is_empty(Rng2I32 rng) {
  return rng.max.x < rng.min.x || rng.max.y < rng.min.y;
}

internal B32 rng2i32_contains(Rng2I32 rng, V2I p) {
  return rng.min.x <= p.x && p.x <= rng.max.x &&
         rng.min.y <= p.y && p.y <= rng.max.y;
}

internal Rng2I32 rng2i32_intersect(Rng2I32 a, Rng2I32 b) {
  return (Rng2I32){{Max(a.min.x, b.min.x), Max(a.min.y, b.min.y)},
                   {Min(a.max.x, b.max.x), Min(a.max.y, b.max.y)}};
}

internal U64 rng2i32_area(Rng2I32 rng) {
  if(rng2i32_is_empty(rng)) { return 0; }
  return (U64)(rng.max.x - rng.min.x + 1) * (U64)(rng.max.y - rng.min.y + 1);
}

internal F32 v2_magnitude(V2 a) {
  return sqrtf(a.x * a.x + a.y * a.y);
}

internal V2 v2_norm(V2 a, V2 fallback) {
  F32 mag = v2_magnitude(a);
  if(mag == 0) { return fallback; }
  return (V2){a.x / mag, a.y / mag};
}

internal V4 v4_splat(F32 v) {
  return (V4){v, v, v, v};
}
