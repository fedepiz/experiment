package color

import "core:math"

import "../geo"

////////////////////////////////
//~ fp: Color
//
// Each color in the gfx layer is a geo.V4, which holds r, g, b and a from 0
// to 1. This module makes such a color and combines two of them, and does
// nothing more.
//
// A hue is in turns, so the values from 0 to 1 go around the wheel one time. A
// lerp between two hues, and a hue that moves with the time, therefore need no
// value of 360 in the code that calls this module.

//- fp: constructors

// an alpha of 1
rgb :: proc(r, g, b: f32) -> geo.V4 {
	return {r, g, b, 1.0}
}

// 0xRRGGBBAA
hex :: proc(hex: u32) -> geo.V4 {
	return {
		f32((hex >> 24) & 0xFF) / 255.0,
		f32((hex >> 16) & 0xFF) / 255.0,
		f32((hex >> 8) & 0xFF) / 255.0,
		f32((hex >> 0) & 0xFF) / 255.0,
	}
}

hsva :: proc(h, s, v, a: f32) -> geo.V4 {
	h := h - math.floor(h) // put the value into the range 0 to 1, which is one turn
	s := clamp(s, 0.0, 1.0)
	v := clamp(v, 0.0, 1.0)
	h6 := h * 6.0
	sector := int(h6)
	f := h6 - f32(sector)
	p := v * (1.0 - s)
	q := v * (1.0 - s * f)
	t := v * (1.0 - s * (1.0 - f))
	result := geo.V4{0, 0, 0, a}
	switch sector % 6 {
	case 0: result.r = v; result.g = t; result.b = p
	case 1: result.r = q; result.g = v; result.b = p
	case 2: result.r = p; result.g = v; result.b = t
	case 3: result.r = p; result.g = q; result.b = v
	case 4: result.r = t; result.g = p; result.b = v
	case 5: result.r = v; result.g = p; result.b = q
	}
	return result
}

// The opposite of hsva: .x is h, .y is s, .z is v, and .w is a. The hue of a
// gray is 0.
to_hsva :: proc(color: geo.V4) -> geo.V4 {
	hi := max(color.r, max(color.g, color.b))
	lo := min(color.r, min(color.g, color.b))
	delta := hi - lo
	h: f32 = 0
	if delta > 0 {
		if hi == color.r {
			h = (color.g - color.b) / delta
		} else if hi == color.g {
			h = 2.0 + (color.b - color.r) / delta
		} else {
			h = 4.0 + (color.r - color.g) / delta
		}
		h = h / 6.0
		if h < 0 { h += 1.0 }
	}
	return {h, (hi == 0) ? 0 : (delta / hi), hi, color.a}
}

//- fp: mixing

// one component at a time. t clamps to 0 and 1.
lerp :: proc(a, b: geo.V4, t: f32) -> geo.V4 {
	t := clamp(t, 0.0, 1.0)
	return a + (b - a) * t
}

with_alpha :: proc(color: geo.V4, a: f32) -> geo.V4 {
	return {color.r, color.g, color.b, a}
}

// The final step of splitmix64. Two hashes that follow each other, and an id
// is the usual case, then give two colors that are far apart on the wheel, and
// not two colors that a person cannot tell apart.
@(private)
hash_mix :: proc(hash: u64) -> u64 {
	hash := hash
	hash ~= hash >> 30; hash *= 0xbf58476d1ce4e5b9
	hash ~= hash >> 27; hash *= 0x94d049bb133111eb
	hash ~= hash >> 31
	return hash
}

// A solid color from a hash of 64 bits, which looks random. A hash of 0 always
// gives a transparent color.
rgb_from_hash :: proc(hash: u64) -> geo.V4 {
	if hash == 0 { return TRANSPARENT } // there is nothing to give a color
	bits := hash_mix(hash)
	// The hue takes the whole wheel. The saturation and the value take a band
	// each, and both bands stay easy to see. No result is therefore near black,
	// near white, or of a low saturation.
	h := f32((bits >> 40) & 0xFFFF) / 65536.0
	s := 0.55 + f32((bits >> 24) & 0xFF) / 255.0 * 0.35
	v := 0.65 + f32((bits >> 8) & 0xFF) / 255.0 * 0.30
	return hsva(h, s, v, 1.0)
}

//- fp: constants

WHITE :: geo.V4{1.00, 1.00, 1.00, 1.0}
BLACK :: geo.V4{0.00, 0.00, 0.00, 1.0}
GRAY :: geo.V4{0.50, 0.50, 0.50, 1.0}
RED :: geo.V4{1.00, 0.00, 0.00, 1.0}
GREEN :: geo.V4{0.00, 1.00, 0.00, 1.0}
BLUE :: geo.V4{0.00, 0.00, 1.00, 1.0}
YELLOW :: geo.V4{1.00, 1.00, 0.00, 1.0}
CYAN :: geo.V4{0.00, 1.00, 1.00, 1.0}
MAGENTA :: geo.V4{1.00, 0.00, 1.00, 1.0}
TRANSPARENT :: geo.V4{0.00, 0.00, 0.00, 0.0}
