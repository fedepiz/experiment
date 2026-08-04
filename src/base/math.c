#include "base/core.h"
#include "base/math.h"

internal V2 v2_add(V2 a, V2 b) {
  return (V2){ a.x + b.x, a.y + b.y };
}

internal V2 v2_sub(V2 a, V2 b) {
  return (V2){ a.x - b.x, a.y - b.y };
}

internal V2 v2_scaled_add(V2 a, V2 b, F32 c) {
  return (V2){ a.x + b.x * c, a.y + b.y * c};
}


