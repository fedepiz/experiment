package draw

import "core:math"
import "core:mem"
import "core:mem/virtual"
import "core:os"
import stbi "vendor:stb/image"
import stbtt "vendor:stb/truetype"

import "../geo"
import "../render"

////////////////////////////////
//~ fp: Draw Layer
//
// This layer puts a meaning and a resource on the quads of the render layer:
// a font, an image, a sprite sheet, and the vocabulary of an immediate mode
// drawing interface. This is the one package that imports the render layer.
//
// The sheet holds each resource. A sprite sheet packs its images on shelves,
// as each image arrives, so a position is complete at the push, which is what
// the interface promises. The glyph atlas is one more sheet, and it never
// closes.
//
// Each shape goes through emit_screen, which puts the current clip on the
// quad. emit is the entry for a rect, and it applies the camera transform
// first. line does not use emit, because the quad of a line exists only after
// the transform of its two end points.
//
// Each coordinate is in points, in the space of the window, from the top left
// corner. Each color is a geo.V4 that holds RGBA from 0 to 1. Each call draws
// above the calls before it.
//
// stbtt and stbi call malloc for their temporary memory. That memory stays
// inside those two libraries. Each allocation of this package goes through an
// allocator, as usual.

////////////////////////////////
//~ fp: State

@(private) SHEET_CAP :: 64
@(private) FONT_CAP :: 16
@(private) CLIP_STACK_CAP :: 64
@(private) SHEET_DEFAULT_DIM :: 2048

@(private)
Sheet :: struct {
	used: bool,
	tex: render.Handle,
	w: int,
	h: int,
	//- the packer of the shelves
	shelf_x: int,
	shelf_y: int,
	shelf_h: int,
}

@(private)
Glyph_Key :: struct {
	codepoint: rune,
	size_px: int, // The height in pixels of the raster. It is part of the key of the cache.
}

@(private)
Glyph :: struct {
	has_bitmap: bool, // A space moves the pen and draws nothing.
	src: geo.Rect,    // the texels in the glyph sheet
	w_px, h_px: f32,
	xoff_px, yoff_px: f32, // the offset of the bitmap from the pen, in pixels
	advance_px: f32,
}

@(private)
Font_Slot :: struct {
	used: bool,
	ttf: []u8,
	info: stbtt.fontinfo,
	glyphs: map[Glyph_Key]Glyph,
}

@(private)
State :: struct {
	initialized: bool,
	arena: virtual.Arena, // It holds the font files and the glyph caches, for the whole program.
	allocator: mem.Allocator,

	//- the members of one frame
	frame_allocator: mem.Allocator,
	in_frame: bool,
	scale: f32,
	viewport_pts: geo.V2,

	//- The stack of clips. Each entry is the intersection with the entry below
	//  it, so a read needs no further work.
	clip_stack: [dynamic;CLIP_STACK_CAP]geo.Rect,

	//- the camera
	camera: Camera,
	camera_active: bool,

	//- the sheets
	sheets: [SHEET_CAP]Sheet,
	open_sheet: u64,  // The slot plus 1 of the sheet that the code builds now. 0 says that there is none.
	glyph_sheet: u64, // The slot plus 1. The first raster of a glyph makes this sheet.

	//- the fonts
	fonts: [FONT_CAP]Font_Slot,
}

@(private) state: State

////////////////////////////////
//~ fp: Frame Lifecycle
//
// These functions hold the frame of the render layer. The application calls
// the draw layer, and the draw layer calls the render layer.

init :: proc() {
	render.init()
	if virtual.arena_init_growing(&state.arena) != nil { panic("out of memory: draw arena") }
	state.allocator = virtual.arena_allocator(&state.arena)
	state.initialized = true
}

frame_begin :: proc(frame_allocator: mem.Allocator, framebuffer_size_px: geo.V2, points_to_pixels: f32) {
	state.frame_allocator = frame_allocator
	state.in_frame = true
	state.scale = (points_to_pixels == 0) ? 1.0 : points_to_pixels // A scale of 0 reads as 1 (ZII).
	state.viewport_pts = framebuffer_size_px / state.scale
	state.clip_stack = {}
	state.camera_active = false
	render.frame_begin(frame_allocator, framebuffer_size_px, state.scale)
}

frame_end :: proc() {
	assert(len(state.clip_stack) == 0) // a push_clip with no pop_clip
	assert(!state.camera_active)  // a camera_begin with no camera_end
	render.frame_end()
	state.frame_allocator = {}
	state.in_frame = false
}

////////////////////////////////
//~ fp: Camera + Quad Emission
//
// A camera is a value. The state of the game holds one and moves it. The same
// struct serves two uses that do not depend on each other: it makes the draws
// between begin and end read world coordinates, and it converts one point,
// which the code for input and for a UI that follows the world uses without
// any draw.
//
// There are two spaces: the world space, inside begin and end, and the screen
// space, which is the default. There is no stack of transforms, and a camera
// inside a camera is not possible.
//
// The CPU applies the transform at the push, so the render layer holds no
// camera, and a camera cannot end a batch. A clip is in screen space
// everywhere. To clip to an area of the world, convert the corners of that
// area and push the result.
//
// A camera does not turn for now. A turn would make each rect of the world
// unaligned to the axes, against the rules for a corner radius and a border.
//
// Text in world space changes size as the geometry does, so a zoom makes the
// glyphs larger and does not draw them again at the new size. A later change
// inside text can correct that, and the interface stays the same.

Camera :: struct {
	center: geo.V2, // the point of the world at the center of the viewport
	zoom: f32,      // The points of the screen for each unit of the world. A zoom of
	                // 0 reads as 1, so the zero camera is the identity camera at the
	                // origin (ZII).

	// The state of the movement with inertia. The code that steers the camera
	// holds it. The transform above does not read it. A zero camera stands
	// still (ZII).
	pan_vel: geo.V2, // the units of the world for each second
	zoom_vel: f32,   // the doublings for each second
}

@(private)
camera_zoom :: proc(cam: Camera) -> f32 {
	return (cam.zoom == 0) ? 1.0 : cam.zoom // The zero camera is the identity camera (ZII).
}

// These two functions read the camera value and the size of the viewport, and
// do nothing more. The size comes from the current frame, so between two
// frames a conversion uses the frame that began last. A resize makes that size
// one frame old at worst.
camera_to_screen :: proc(cam: Camera, world_point: geo.V2) -> geo.V2 {
	zoom := camera_zoom(cam)
	return (world_point - cam.center) * zoom + 0.5 * state.viewport_pts
}

camera_from_screen :: proc(cam: Camera, screen_point: geo.V2) -> geo.V2 {
	zoom := camera_zoom(cam)
	return (screen_point - 0.5 * state.viewport_pts) / zoom + cam.center
}

// each draw between the two is in world space
camera_begin :: proc(cam: Camera) {
	state.camera = cam
	state.camera_active = true
}

// go back to screen space
camera_end :: proc() {
	state.camera_active = false
}

// A quad in screen space. This function applies the clip and nothing more.
@(private)
emit_screen :: proc(quad: ^render.Quad) {
	if len(state.clip_stack) > 0 {
		quad.clip = state.clip_stack[len(state.clip_stack) - 1]
	}
	render.push_quad(quad)
}

// A quad that can be in world space. This function applies the camera
// transform first. The geometry changes size with the zoom. A rotation does
// not change with a scale, so it passes through.
@(private)
emit :: proc(quad: ^render.Quad) {
	if state.camera_active {
		zoom := camera_zoom(state.camera)
		quad.dst.min = camera_to_screen(state.camera, quad.dst.min)
		quad.dst.max = camera_to_screen(state.camera, quad.dst.max)
		for corner in geo.Corner {
			quad.corner_radii[corner] *= zoom
		}
		quad.border_thickness *= zoom
		quad.edge_softness *= zoom
	}
	emit_screen(quad)
}

////////////////////////////////
//~ fp: Clip Stack
//
// A clip applies to each shape that you draw while it is on the stack. A
// second clip inside the first gives the intersection of the two. The clip of
// a quad in the render layer holds the result, so a clip cannot end a batch.

push_clip :: proc(r: geo.Rect) {
	if len(state.clip_stack) < CLIP_STACK_CAP {
		resolved := r
		if len(state.clip_stack) > 0 {
			top := state.clip_stack[len(state.clip_stack) - 1]
			resolved.min.x = max(resolved.min.x, top.min.x)
			resolved.min.y = max(resolved.min.y, top.min.y)
			resolved.max.x = min(resolved.max.x, top.max.x)
			resolved.max.y = min(resolved.max.y, top.max.y)
		}
		if resolved.max.x <= resolved.min.x || resolved.max.y <= resolved.min.y {
			// Two clips that do not overlap remove each fragment. A rect of 0 reads
			// as "no clip" further down, so put this rect where nothing draws.
			resolved = {{-1e8, -1e8}, {-1e8 + 1.0, -1e8 + 1.0}}
		}
		append(&state.clip_stack, resolved)
	}
}

pop_clip :: proc() {
	if len(state.clip_stack) > 0 {
		pop(&state.clip_stack)
	}
}

////////////////////////////////
//~ fp: Rectangles / Lines
//
// rect_ex gives control of each value. The functions below it cover the usual
// cases, and most call sites read better with one of those.

Rect_Params :: struct {
	rect: geo.Rect,
	colors: [geo.Corner]geo.V4, // A color at each corner makes a gradient.
	corner_radii: [geo.Corner]f32,
	border_thickness: f32, // A value of 0 fills the rect.
	edge_softness: f32,    // A value of 0 gives a hard edge.
}

rect_ex :: proc(params: Rect_Params) {
	quad: render.Quad
	quad.dst = params.rect
	quad.colors = params.colors
	quad.corner_radii = params.corner_radii
	quad.border_thickness = params.border_thickness
	quad.edge_softness = params.edge_softness
	emit(&quad)
}

rect :: proc(r: geo.Rect, color: geo.V4) {
	params: Rect_Params
	params.rect = r
	for corner in geo.Corner { params.colors[corner] = color }
	rect_ex(params)
}

rect_rounded :: proc(r: geo.Rect, color: geo.V4, radius: f32) {
	params: Rect_Params
	params.rect = r
	for corner in geo.Corner {
		params.colors[corner] = color
		params.corner_radii[corner] = radius
	}
	params.edge_softness = 1.0 // A curve needs a smooth edge.
	rect_ex(params)
}

rect_outline :: proc(r: geo.Rect, color: geo.V4, thickness: f32) {
	params: Rect_Params
	params.rect = r
	for corner in geo.Corner { params.colors[corner] = color }
	params.border_thickness = thickness
	params.edge_softness = 1.0
	rect_ex(params)
}

// a gradient from the top to the bottom
rect_gradient_v :: proc(r: geo.Rect, top, bottom: geo.V4) {
	params: Rect_Params
	params.rect = r
	params.colors[.TL] = top
	params.colors[.TR] = top
	params.colors[.BL] = bottom
	params.colors[.BR] = bottom
	rect_ex(params)
}

line :: proc(a, b: geo.V2, thickness: f32, color: geo.V4) {
	// The two end points convert as points. The code then builds the quad in
	// screen space.
	a := a
	b := b
	zoom: f32 = 1.0
	if state.camera_active {
		zoom = camera_zoom(state.camera)
		a = camera_to_screen(state.camera, a)
		b = camera_to_screen(state.camera, b)
	}
	d := b - a
	length := math.sqrt(d.x * d.x + d.y * d.y)
	if length > 0 {
		half_len := 0.5 * length
		half_thick := 0.5 * thickness * zoom
		center := 0.5 * (a + b)
		quad: render.Quad
		quad.dst = {{center.x - half_len, center.y - half_thick},
		            {center.x + half_len, center.y + half_thick}}
		for corner in geo.Corner { quad.colors[corner] = color }
		// The rotation of a quad is counter-clockwise for a person who looks at
		// the screen. y grows toward the bottom, so the angle takes a minus sign.
		quad.rotation = math.atan2(-d.y, d.x)
		quad.edge_softness = 1.0
		emit_screen(&quad)
	}
}

////////////////////////////////
//~ fp: Sheets

@(private)
sheet_from_id :: proc(id: u64) -> ^Sheet {
	result: ^Sheet
	if id != 0 && id <= SHEET_CAP && state.sheets[id - 1].used {
		result = &state.sheets[id - 1]
	}
	return result
}

// Take a slot for a sheet, and make the texture of that sheet on the GPU.
// opt_data writes the texture as it is, which suits a sheet of one image at
// the size of that image. With no data the texture holds zeros. A pusher that
// leaves the gutter ring empty, such as the pusher of a glyph, needs a
// transparent black there, and not the bytes that the memory held.
@(private)
sheet_create :: proc(w, h: int, format: render.Tex_Format, sampling: Sampling, opt_data: []u8) -> u64 {
	result: u64
	slot := SHEET_CAP
	for i in 0 ..< SHEET_CAP {
		if !state.sheets[i].used {
			slot = i
			break
		}
	}
	if slot < SHEET_CAP && w > 0 && h > 0 {
		tex_sampling: render.Tex_Sampling = (sampling == .Pixel) ? .Nearest : .Linear
		tex: render.Handle
		if opt_data != nil {
			tex = render.tex_alloc(w, h, format, tex_sampling, opt_data)
		} else {
			texel_size := (format == .R8) ? 1 : 4
			zeroes := make([]u8, w * h * texel_size, context.temp_allocator)
			tex = render.tex_alloc(w, h, format, tex_sampling, zeroes)
		}
		if tex != 0 {
			sheet := &state.sheets[slot]
			sheet^ = {}
			sheet.used = true
			sheet.tex = tex
			sheet.w = w
			sheet.h = h
			result = u64(slot + 1)
		}
	}
	return result
}

// The packer puts each image on a shelf as that image arrives. The position
// is complete when the packer gives it back, which is what makes a sprite
// complete at the push.
//
// Each region takes a ring of GUTTER texels around its content, so linear
// sampling at the edge of a sprite cannot reach the region next to it.
//
// The pusher decides what the ring holds. An image copies its edge texels into
// the ring, so two tiles meet with no seam. A glyph leaves the ring
// transparent black, which is correct for text with an alpha blend.
@(private) GUTTER :: 1

@(private)
sheet_pack :: proc(sheet: ^Sheet, w, h: int) -> (x, y: int, ok: bool) {
	fw := w + 2 * GUTTER
	fh := h + 2 * GUTTER
	if sheet.shelf_x + fw > sheet.w { // The shelf has no space. Start the next shelf.
		sheet.shelf_y += sheet.shelf_h
		sheet.shelf_x = 0
		sheet.shelf_h = 0
	}
	if sheet.shelf_x + fw <= sheet.w && sheet.shelf_y + fh <= sheet.h {
		x = sheet.shelf_x + GUTTER
		y = sheet.shelf_y + GUTTER
		sheet.shelf_x += fw
		sheet.shelf_h = max(sheet.shelf_h, fh)
		ok = true
	}
	return
}

////////////////////////////////
//~ fp: Images (CPU) / Sprites & Sheets (GPU)
//
// An Image is a block of decoded pixels on an allocator, and nothing more.
//
// An image goes to the GPU in a sprite sheet. The build of a sheet is a phase
// with a start and an end, between spritesheet_begin and spritesheet_end.
// Build a sheet at load time, and not inside a frame. Each image that you push
// in that phase goes into the sheet, and comes back as a Sprite.
//
// The draw layer names no texture. The sheet is the texture, and a sprite is
// the one resource that you can draw.
//
// The sheet packs each image as it arrives, on shelves. A sprite is therefore
// complete when the push gives it back, and no later step can change it. Push
// the tall images first for a smaller sheet. Each sheet samples linearly for
// now, and a font does too.

// The image holds RGBA8. The caller does not read a byte: the functions below
// read and write a pixel as a geo.V4 color from 0 to 1.
Image :: struct {
	w: int,
	h: int,
	pixels: []u8, // RGBA8, which is w*h*4 bytes
}

@(private)
u8_from_unit :: proc(x: f32) -> u8 {
	return u8(clamp(x, 0.0, 1.0) * 255.0 + 0.5)
}

// A file that is absent, and a file that the decoder cannot read, both give
// the zero image (ZII). The decoder reads each format of stb_image: png, jpg,
// bmp, tga, gif and more.
image_load :: proc(path: string, allocator := context.allocator) -> Image {
	result: Image
	file, read_err := os.read_entire_file(path, context.temp_allocator)
	if read_err == nil && len(file) > 0 {
		w, h, source_comp: i32
		decoded := stbi.load_from_memory(raw_data(file), i32(len(file)), &w, &h, &source_comp, 4)
		if decoded != nil {
			result.w = int(w)
			result.h = int(h)
			result.pixels = make([]u8, result.w * result.h * 4, allocator)
			copy(result.pixels, decoded[:result.w * result.h * 4])
			stbi.image_free(decoded) // stbi allocated it with malloc. The copy is on the allocator.
		}
	}
	return result
}

image_create :: proc(w, h: int, color: geo.V4, allocator := context.allocator) -> Image {
	result: Image
	if w > 0 && h > 0 {
		result.w = w
		result.h = h
		result.pixels = make([]u8, w * h * 4, allocator)
		rgba := [4]u8{
			u8_from_unit(color.r),
			u8_from_unit(color.g),
			u8_from_unit(color.b),
			u8_from_unit(color.a),
		}
		for i in 0 ..< w * h {
			copy(result.pixels[i * 4:], rgba[:])
		}
	}
	return result
}

// a position outside the image, and a nil image, give {0}
image_get_px :: proc(image: Image, x, y: int) -> geo.V4 {
	result: geo.V4
	if image.pixels != nil && 0 <= x && x < image.w && 0 <= y && y < image.h {
		px := image.pixels[(y * image.w + x) * 4:]
		result.r = f32(px[0]) / 255.0
		result.g = f32(px[1]) / 255.0
		result.b = f32(px[2]) / 255.0
		result.a = f32(px[3]) / 255.0
	}
	return result
}

// a position outside the image, and a nil image, write nothing
image_set_px :: proc(image: Image, x, y: int, color: geo.V4) {
	if image.pixels != nil && 0 <= x && x < image.w && 0 <= y && y < image.h {
		px := image.pixels[(y * image.w + x) * 4:]
		px[0] = u8_from_unit(color.r)
		px[1] = u8_from_unit(color.g)
		px[2] = u8_from_unit(color.b)
		px[3] = u8_from_unit(color.a)
	}
}

// Copy src into dst at (x, y), and clip it. A nil image on either side copies
// nothing.
image_blit :: proc(dst: Image, x, y: int, src: Image) {
	if dst.pixels != nil && src.pixels != nil {
		// Clip src against dst. Each row that stays copies in full.
		x0 := max(x, 0)
		y0 := max(y, 0)
		x1 := min(x + src.w, dst.w)
		y1 := min(y + src.h, dst.h)
		for dy in y0 ..< y1 {
			if x0 >= x1 { break }
			copy(dst.pixels[(dy * dst.w + x0) * 4:],
			     src.pixels[((dy - y) * src.w + (x0 - x)) * 4:][:(x1 - x0) * 4])
		}
	}
}

//- fp: sprite sheets

Sprite_Sheet :: distinct u64 // the id of the sheet, which the draw layer alone reads

// A sprite is a value, and not a handle to a hidden object. It says which
// sheet holds it, where it is in that sheet, and how large it is.
// spritesheet_push gives a complete sprite, which no later step changes.
// `size` is the member that a caller reads. The other members address the
// texels.
Sprite :: struct {
	sheet: Sprite_Sheet,
	src: geo.Rect, // the texels inside the sheet
	size: geo.V2,  // The texels. It is the size of src, for the convenience of the caller.
}

// How the texels of a sheet sample on the screen. The sheet takes this value
// when it starts, and the value does not change.
Sampling :: enum {
	Smooth, // linear, which is the default at a value of 0
	Pixel,  // nearest. Pixel art stays hard at each zoom.
}

// w and h are the texels of the sheet. The caller chooses that size, because
// the sheet packs each image as it arrives, and therefore allocates before the
// content comes. A size of 0 gives 2048 (ZII).
spritesheet_begin :: proc(w, h: int, sampling: Sampling) {
	assert(state.open_sheet == 0) // the code can build one sheet at a time
	w := (w == 0) ? SHEET_DEFAULT_DIM : w // A size of 0 reads as the default (ZII).
	h := (h == 0) ? SHEET_DEFAULT_DIM : h
	state.open_sheet = sheet_create(w, h, .RGBA8, sampling, nil)
}

// A zero image, and a sheet with no space, give a nil sprite (ZII).
spritesheet_push :: proc(image: Image) -> Sprite {
	result: Sprite
	sheet := sheet_from_id(state.open_sheet)
	if sheet != nil && image.pixels != nil && image.w > 0 && image.h > 0 {
		if x, y, ok := sheet_pack(sheet, image.w, image.h); ok {
			// Send the image and its gutter ring. The ring holds a copy of the edge
			// texels of the image. Linear sampling at the border of the sprite then
			// mixes the colors of that sprite alone, so two tiles meet with no
			// seam.
			fw := image.w + 2 * GUTTER
			fh := image.h + 2 * GUTTER
			staging := make([]u8, fw * fh * 4, context.temp_allocator)
			for sy in 0 ..< fh {
				cy := clamp(sy - GUTTER, 0, image.h - 1)
				for sx in 0 ..< fw {
					cx := clamp(sx - GUTTER, 0, image.w - 1)
					copy(staging[(sy * fw + sx) * 4:],
					     image.pixels[(cy * image.w + cx) * 4:][:4])
				}
			}
			render.tex_update(sheet.tex, x - GUTTER, y - GUTTER, fw, fh, staging)

			result.sheet = Sprite_Sheet(state.open_sheet)
			result.src = {{f32(x), f32(y)}, {f32(x + image.w), f32(y + image.h)}}
			result.size = {f32(image.w), f32(image.h)}
		}
		// The sheet has no space. The function then gives the nil sprite (ZII).
	}
	return result
}

// Push one rectangular window of a canvas that wraps on both axes. The
// coordinates are texels.
//
// spritesheet_push puts empty texels in the gutter around a sprite. This
// function puts the texels of the canvas that follow the window there, and
// wraps at each edge of the canvas. Two windows of one continuous texture
// therefore meet with no seam under linear sampling. Without those texels each
// joint becomes a thin line, and the joints together read as a grid.
//
// A window that covers the whole canvas gives one sprite that tiles against
// itself. A nil canvas, and an empty window, give a nil sprite (ZII).
spritesheet_push_window :: proc(canvas: Image, window: geo.Rect) -> Sprite {
	result: Sprite
	sheet := sheet_from_id(state.open_sheet)
	x0 := int(window.min.x)
	y0 := int(window.min.y)
	w := int(window.max.x) - x0
	h := int(window.max.y) - y0
	if sheet != nil && canvas.pixels != nil && w > 0 && h > 0 {
		if x, y, ok := sheet_pack(sheet, w, h); ok {
			// Send the window and its gutter ring. The ring holds the texels of the
			// canvas that follow the window, and it wraps at each edge of the
			// canvas. Linear sampling at the border of the sprite then mixes into
			// the true neighbour of the window, so two parts of one texture that
			// wraps meet with no seam on the screen. A ring of copied edge texels,
			// which spritesheet_push makes, would give each joint a thin line, and
			// those lines together read as a grid. A window that covers the whole
			// canvas gives one sprite that tiles against itself.
			fw := w + 2 * GUTTER
			fh := h + 2 * GUTTER
			staging := make([]u8, fw * fh * 4, context.temp_allocator)
			for sy in 0 ..< fh {
				cy := (y0 + sy - GUTTER + canvas.h) % canvas.h
				for sx in 0 ..< fw {
					cx := (x0 + sx - GUTTER + canvas.w) % canvas.w
					copy(staging[(sy * fw + sx) * 4:],
					     canvas.pixels[(cy * canvas.w + cx) * 4:][:4])
				}
			}
			render.tex_update(sheet.tex, x - GUTTER, y - GUTTER, fw, fh, staging)

			result.sheet = Sprite_Sheet(state.open_sheet)
			result.src = {{f32(x), f32(y)}, {f32(x + w), f32(y + h)}}
			result.size = {f32(w), f32(h)}
		}
	}
	return result
}

spritesheet_end :: proc() -> Sprite_Sheet {
	result: Sprite_Sheet
	sheet := sheet_from_id(state.open_sheet)
	if sheet != nil {
		result = Sprite_Sheet(state.open_sheet)
	}
	state.open_sheet = 0
	return result
}

// Each sprite of that sheet then draws nothing.
spritesheet_release :: proc(id: Sprite_Sheet) {
	sheet := sheet_from_id(u64(id))
	if sheet != nil {
		render.tex_release(sheet.tex)
		sheet^ = {}
	}
}

// A sheet of one image, in one call, at the size of that image. Use it for an
// image that stands alone, such as a background.
sprite_from_image :: proc(image: Image) -> Sprite {
	result: Sprite
	if image.pixels != nil && image.w > 0 && image.h > 0 {
		// A sheet at the size of this image. There is no packing, and the texture
		// is the sprite.
		id := sheet_create(image.w, image.h, .RGBA8, .Smooth, image.pixels)
		if id != 0 {
			result.sheet = Sprite_Sheet(id)
			result.src = {{0, 0}, {f32(image.w), f32(image.h)}}
			result.size = {f32(image.w), f32(image.h)}
		}
	}
	return result
}

//- fp: sprite drawing

Sprite_Params :: struct {
	sprite: Sprite,
	dst: geo.Rect, // points
	src: geo.Rect, // The texels, in the space of the sprite. A rect of 0 gives the whole sprite.
	tint: geo.V4,  // It multiplies the color of the sprite. White keeps that color.
	rotation: f32, // radians, counter-clockwise, about the center of dst
	mask: Sprite,  // An alpha mask across dst. The alpha of the sprite multiplies
	               // by the alpha of the mask at each pixel. The mask must be in
	               // the sheet of the sprite. A nil mask masks nothing.
}

sprite_ex :: proc(params: Sprite_Params) {
	sheet := sheet_from_id(u64(params.sprite.sheet))
	if sheet != nil { // A nil sprite, and a sheet that is gone, draw nothing.
		quad: render.Quad
		quad.dst = params.dst
		quad.tex = sheet.tex
		quad.rotation = params.rotation

		// src is in the texel space of the sprite. A rect of 0 gives the whole
		// sprite.
		src := params.src
		if src.max.x <= src.min.x || src.max.y <= src.min.y {
			src = {{0, 0}, {params.sprite.size.x, params.sprite.size.y}}
		}
		quad.src.min = params.sprite.src.min + src.min
		quad.src.max = params.sprite.src.min + src.max

		// The mask is on the texture of this same sheet, which sprite_masked
		// asserts. Its rect in the sheet therefore passes through with no
		// change.
		if params.mask.sheet != 0 {
			quad.mask_src = params.mask.src
		}

		// A tint of 0 reads as white, which keeps the colors of the sprite (ZII).
		tint := params.tint
		if tint == {} {
			tint = {1, 1, 1, 1}
		}
		for corner in geo.Corner { quad.colors[corner] = tint }
		emit(&quad)
	}
}

sprite :: proc(s: Sprite, dst: geo.Rect, tint: geo.V4) {
	params: Sprite_Params
	params.sprite = s
	params.dst = dst
	params.tint = tint
	sprite_ex(params)
}

// Draw a sprite through an alpha mask. The boundary between two terrains uses
// this function most. An assert tests that the mask is in the sheet of the
// sprite: a quad reads one texture, so a mask cannot end a batch.
sprite_masked :: proc(s: Sprite, mask: Sprite, dst: geo.Rect, tint: geo.V4) {
	// A quad reads one texture. A mask that is not nil must therefore be in the
	// sheet of the sprite. Without that rule the shader would read the rect of
	// the mask from the wrong sheet. A nil mask is correct, and it draws the
	// sprite with no mask (ZII).
	assert(mask.sheet == 0 || mask.sheet == s.sheet)
	params: Sprite_Params
	params.sprite = s
	params.dst = dst
	params.tint = tint
	params.mask = mask
	sprite_ex(params)
}

////////////////////////////////
//~ fp: Fonts / Text
//
// A font is a handle into the font cache of the draw layer.
//
// A glyph goes into an internal sprite sheet when a call draws it the first
// time, at the size and the scale of the screen of that call. That sheet stays
// open, because the set of glyphs grows while the program runs, and a phase
// with a start and an end cannot hold it. A caller does not see that sheet.
//
// A position of text is the TOP LEFT corner of the box around that text, which
// suits a layout. Use font_metrics where the baseline is what you need. Each
// size is in points, as in "text of 14 points". Each string is UTF-8.

Font :: distinct u64

@(private)
font_from_handle :: proc(font: Font) -> ^Font_Slot {
	result: ^Font_Slot
	if font != 0 && int(font) <= FONT_CAP && state.fonts[font - 1].used {
		result = &state.fonts[font - 1]
	}
	return result
}

// A file that is absent, and a file that the parser cannot read, give a nil
// font, which draws nothing (ZII).
font_open :: proc(path: string) -> Font {
	result: Font
	if state.initialized {
		slot := FONT_CAP
		for i in 0 ..< FONT_CAP {
			if !state.fonts[i].used {
				slot = i
				break
			}
		}
		if slot < FONT_CAP {
			ttf, read_err := os.read_entire_file(path, state.allocator)
			if read_err == nil && len(ttf) > 0 {
				font := &state.fonts[slot]
				// GetFontOffsetForIndex reads a plain .ttf file and a .ttc collection.
				offset := stbtt.GetFontOffsetForIndex(raw_data(ttf), 0)
				if offset >= 0 && bool(stbtt.InitFont(&font.info, raw_data(ttf), offset)) {
					font.used = true
					font.ttf = ttf
					font.glyphs = make(map[Glyph_Key]Glyph, state.allocator)
					result = Font(slot + 1)
				}
			}
		}
	}
	return result
}

font_close :: proc(font: Font) {
	slot := font_from_handle(font)
	if slot != nil {
		// The bytes of the font and the glyph cache stay on the arena of the
		// whole program. A font closes seldom, so the memory that a close gives
		// back does not justify an allocator.
		slot^ = {}
	}
}

Font_Metrics :: struct {
	ascent: f32,       // from the top of the box to the baseline
	descent: f32,      // from the baseline to the bottom of the box
	line_advance: f32, // from one baseline to the next baseline
}

font_metrics :: proc(font: Font, size: f32) -> Font_Metrics {
	result: Font_Metrics
	slot := font_from_handle(font)
	if slot != nil {
		ascent, descent, line_gap: i32
		stbtt.GetFontVMetrics(&slot.info, &ascent, &descent, &line_gap)
		scale := stbtt.ScaleForPixelHeight(&slot.info, size) // the units are points
		result.ascent = f32(ascent) * scale
		result.descent = f32(-descent) * scale // the descent of stbtt is below 0
		result.line_advance = f32(ascent - descent + line_gap) * scale
	}
	return result
}

// Read the cache, and make the raster where the cache has no entry. A glyph
// goes into the glyph sheet, which never closes, at the size in pixels of the
// first call that draws it.
@(private)
glyph_get :: proc(font: ^Font_Slot, codepoint: rune, size_px: int) -> Glyph {
	key := Glyph_Key{codepoint, size_px}
	if cached, ok := font.glyphs[key]; ok {
		return cached
	}
	glyph: Glyph

	scale := stbtt.ScaleForPixelHeight(&font.info, f32(size_px))
	advance, lsb: i32
	stbtt.GetCodepointHMetrics(&font.info, codepoint, &advance, &lsb)
	glyph.advance_px = f32(advance) * scale

	x0, y0, x1, y1: i32
	stbtt.GetCodepointBitmapBox(&font.info, codepoint, scale, scale, &x0, &y0, &x1, &y1)
	w := int(x1 - x0)
	h := int(y1 - y0)
	if w > 0 && h > 0 {
		if state.glyph_sheet == 0 {
			state.glyph_sheet = sheet_create(SHEET_DEFAULT_DIM, SHEET_DEFAULT_DIM, .R8, .Smooth, nil)
		}
		sheet := sheet_from_id(state.glyph_sheet)
		if sheet != nil {
			if px, py, ok := sheet_pack(sheet, w, h); ok {
				bitmap := make([]u8, w * h, context.temp_allocator)
				stbtt.MakeCodepointBitmap(&font.info, raw_data(bitmap), i32(w), i32(h), i32(w), scale, scale, codepoint)
				render.tex_update(sheet.tex, px, py, w, h, bitmap)
				glyph.has_bitmap = true
				glyph.src = {{f32(px), f32(py)}, {f32(px + w), f32(py + h)}}
				glyph.w_px = f32(w)
				glyph.h_px = f32(h)
				glyph.xoff_px = f32(x0)
				glyph.yoff_px = f32(y0)
			}
			// The glyph sheet has no space. The cache then holds a glyph with no
			// bitmap, which moves the pen and draws nothing.
		}
	}
	font.glyphs[key] = glyph
	return glyph
}

// The walk that the two callers share. It moves the pen across the text, and
// it can draw the quad of each glyph. text and text_dim call it. The walk
// reads the string as UTF-8, one rune at a time, and a wrong byte sequence
// gives U+FFFD and moves one byte, which is the language's own rule.
@(private)
text_walk :: proc(font: Font, size: f32, pos: geo.V2, color: geo.V4, s: string, emit_quads: bool) -> f32 {
	result := pos.x
	slot := font_from_handle(font)
	if slot != nil && size > 0 {
		to_px: f32 = (state.scale == 0) ? 1.0 : state.scale // A call to text_dim can occur before the first frame.
		size_px := int(size * to_px + 0.5)
		scale_px := stbtt.ScaleForPixelHeight(&slot.info, f32(size_px))

		// The walk uses units of points multiplied by the scale. Those units are
		// true pixels for text in screen space. Inside a camera they are units of
		// the world, and the step to the grid of pixels below is then approximate.
		// pos is the top left corner of the box, and each glyph hangs from the
		// baseline.
		baseline := (pos.y + font_metrics(font, size).ascent) * to_px
		pen := pos.x * to_px
		prev_cp: rune = 0
		for cp in s {
			if prev_cp != 0 {
				pen += f32(stbtt.GetCodepointKernAdvance(&slot.info, prev_cp, cp)) * scale_px
			}
			glyph := glyph_get(slot, cp, size_px)
			if emit_quads && glyph.has_bitmap && state.in_frame {
				// Move the glyph to the grid of pixels. Each texel of the glyph then
				// covers one pixel, which keeps the text sharp under linear
				// sampling.
				gx := math.round(pen + glyph.xoff_px)
				gy := math.round(baseline + glyph.yoff_px)
				quad: render.Quad
				quad.dst = {{gx / to_px, gy / to_px},
				            {(gx + glyph.w_px) / to_px, (gy + glyph.h_px) / to_px}}
				quad.src = glyph.src
				quad.tex = sheet_from_id(state.glyph_sheet).tex
				for corner in geo.Corner { quad.colors[corner] = color }
				emit(&quad)
			}
			pen += glyph.advance_px
			prev_cp = cp
		}
		result = pen / to_px
	}
	return result
}

// The size of the box around the text. A layout starts from this function.
text_dim :: proc(font: Font, size: f32, s: string) -> geo.V2 {
	result: geo.V2
	slot := font_from_handle(font)
	if slot != nil {
		result.x = text_walk(font, size, {}, {}, s, false)
		metrics := font_metrics(font, size)
		result.y = metrics.ascent + metrics.descent
	}
	return result
}

// It gives the x of the pen after the last glyph, so a second call can
// continue the line. Use that to draw a line in more than one color, or in
// more than one style.
text :: proc(font: Font, size: f32, pos: geo.V2, color: geo.V4, s: string) -> f32 {
	return text_walk(font, size, pos, color, s, true)
}
