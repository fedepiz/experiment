#include "base/core.h"
#include "base/math.h"
#include "gfx/color.h"

#include <math.h>

////////////////////////////////
//~ fp: Color

internal V4 col_rgba(F32 r, F32 g, F32 b, F32 a) {
  V4 result = {r, g, b, a};
  return result;
}

internal V4 col_rgb(F32 r, F32 g, F32 b) {
  V4 result = {r, g, b, 1.0f};
  return result;
}

internal V4 col_hex(U32 hex) {
  V4 result = {
    (F32)((hex >> 24) & 0xFF) / 255.0f,
    (F32)((hex >> 16) & 0xFF) / 255.0f,
    (F32)((hex >>  8) & 0xFF) / 255.0f,
    (F32)((hex >>  0) & 0xFF) / 255.0f,
  };
  return result;
}

internal V4 col_hsva(F32 h, F32 s, F32 v, F32 a) {
  h = h - floorf(h); // wrap into 0..1 (turns)
  s = Clamp(0.0f, s, 1.0f);
  v = Clamp(0.0f, v, 1.0f);
  F32 h6 = h * 6.0f;
  I32 sector = (I32)h6;
  F32 f = h6 - (F32)sector;
  F32 p = v * (1.0f - s);
  F32 q = v * (1.0f - s * f);
  F32 t = v * (1.0f - s * (1.0f - f));
  V4 result = {0, 0, 0, a};
  switch(sector % 6) {
    case 0: result.x = v; result.y = t; result.z = p; break;
    case 1: result.x = q; result.y = v; result.z = p; break;
    case 2: result.x = p; result.y = v; result.z = t; break;
    case 3: result.x = p; result.y = q; result.z = v; break;
    case 4: result.x = t; result.y = p; result.z = v; break;
    case 5: result.x = v; result.y = p; result.z = q; break;
  }
  return result;
}

internal V4 col_to_hsva(V4 color) {
  F32 max = Max(color.x, Max(color.y, color.z));
  F32 min = Min(color.x, Min(color.y, color.z));
  F32 delta = max - min;
  F32 h = 0;
  if(delta > 0) {
    if(max == color.x)      { h = (color.y - color.z) / delta; }
    else if(max == color.y) { h = 2.0f + (color.z - color.x) / delta; }
    else                    { h = 4.0f + (color.x - color.y) / delta; }
    h = h / 6.0f;
    if(h < 0) { h += 1.0f; }
  }
  V4 result = {h, (max == 0) ? 0 : (delta / max), max, color.w};
  return result;
}

internal V4 col_lerp(V4 a, V4 b, F32 t) {
  t = Clamp(0.0f, t, 1.0f);
  V4 result = {
    a.x + (b.x - a.x) * t,
    a.y + (b.y - a.y) * t,
    a.z + (b.z - a.z) * t,
    a.w + (b.w - a.w) * t,
  };
  return result;
}

internal V4 col_with_alpha(V4 color, F32 a) {
  V4 result = {color.x, color.y, color.z, a};
  return result;
}
