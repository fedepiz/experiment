#pragma once
#include "base/core.h"
#include "base/math.h"

////////////////////////////////
//~ fp: Color
//
// Each color in the gfx layer is a V4, which holds r, g, b and a from 0 to 1.
// This module makes such a color and combines two of them, and does nothing
// more.
//
// A hue is in turns, so the values from 0 to 1 go around the wheel one time. A
// lerp between two hues, and a hue that moves with the time, therefore need no
// value of 360 in the code that calls this module.

//- fp: constructors
internal V4 col_rgba(F32 r, F32 g, F32 b, F32 a);
internal V4 col_rgb(F32 r, F32 g, F32 b); // an alpha of 1
internal V4 col_hex(U32 hex);             // 0xRRGGBBAA
internal V4 col_hsva(F32 h, F32 s, F32 v, F32 a);

// The opposite of col_hsva: .x is h, .y is s, .z is v, and .w is a. The hue of
// a gray is 0.
internal V4 col_to_hsva(V4 color);

//- fp: mixing
internal V4 col_lerp(V4 a, V4 b, F32 t); // one component at a time. t clamps to 0 and 1.
internal V4 col_with_alpha(V4 color, F32 a);

// A solid color from a hash of 64 bits, which looks random. A hash of 0 always
// gives a transparent color.
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
