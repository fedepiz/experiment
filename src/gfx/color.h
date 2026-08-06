#pragma once
#include "base/core.h"
#include "base/math.h"

////////////////////////////////
//~ fp: Color
//
// Colors are plain V4s (r, g, b, a in 0..1) everywhere in gfx; this module
// is just vocabulary for making and combining them. Hue is in turns (0..1
// wraps around the wheel), so hues compose with lerp and time without 360s
// sprinkled through calling code.

//- fp: constructors
internal V4 col_rgba(F32 r, F32 g, F32 b, F32 a);
internal V4 col_rgb(F32 r, F32 g, F32 b); // alpha 1
internal V4 col_hex(U32 hex);             // 0xRRGGBBAA
internal V4 col_hsva(F32 h, F32 s, F32 v, F32 a);

// inverse of col_hsva: .x=h .y=s .z=v .w=a (h is 0 for grays)
internal V4 col_to_hsva(V4 color);

//- fp: mixing
internal V4 col_lerp(V4 a, V4 b, F32 t); // component-wise, t clamped to 0..1
internal V4 col_with_alpha(V4 color, F32 a);

//- fp: returns a pseudo-random solid colour given a 64-bit hash.
// Supplying 0 always returns transparent
internal V4 col_rgb_from_hash(U64 hash);

//- fp: constants
#define Col_White       ((V4){1.00f, 1.00f, 1.00f, 1.0f})
#define Col_Black       ((V4){0.00f, 0.00f, 0.00f, 1.0f})
#define Col_Gray        ((V4){0.50f, 0.50f, 0.50f, 1.0f})
#define Col_Red         ((V4){1.00f, 0.00f, 0.00f, 1.0f})
#define Col_Green       ((V4){0.00f, 1.00f, 0.00f, 1.0f})
#define Col_Blue        ((V4){0.00f, 0.00f, 1.00f, 1.0f})
#define Col_Yellow      ((V4){1.00f, 1.00f, 0.00f, 1.0f})
#define Col_Cyan        ((V4){0.00f, 1.00f, 1.00f, 1.0f})
#define Col_Magenta     ((V4){1.00f, 0.00f, 1.00f, 1.0f})
#define Col_Transparent ((V4){0.00f, 0.00f, 0.00f, 0.0f})
