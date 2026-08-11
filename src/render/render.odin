package render

import "core:fmt"
import "core:mem"
import gl "vendor:OpenGL"

import "../chunk"
import "../geo"

////////////////////////////////
//~ fp: Render Layer
//
// This layer holds the vocabulary of the GPU, and nothing more: a texture,
// which is a block of bytes, and a batch of quads with a texture. It holds no
// font, no file, and no idea of text or of a sprite. The draw layer above
// holds those.
//
// The interface names no type of the backend. The code below uses OpenGL,
// through vendor:OpenGL, whose loader the window layer runs at equip_gl.
//
// The work of this layer is the batch. Each quad that you push between
// frame_begin and frame_end goes on the frame allocator. frame_end then draws
// the quads as a small number of instanced calls. The caller counts no draw
// calls.
//
// The renderer draws the quads in the order that the caller pushed them, and
// it moves no quad. Alpha blending gives that order a meaning. A batch is the
// longest run of quads that share one texture, so a change of texture is the
// one thing that ends a batch. Each other value of a quad goes with the
// instance.
//
// The geometry of a quad is in points, in the space of the window, from the
// top left corner. The window layer uses that space too. The framebuffer is in
// pixels. frame_begin takes the size in pixels and the scale from points to
// pixels, and puts both into the projection. No code above this line reads the
// size of a pixel.

////////////////////////////////
//~ fp: Handles
//
// The renderer alone reads the value of a handle. The nil handle is 0, as
// with each other type. A handle is a distinct type and not a u64, so two
// kinds of handle cannot convert into each other.

Handle :: distinct u64

////////////////////////////////
//~ fp: Textures

Tex_Format :: enum {
	None,
	R8,    // one byte for each texel. The glyph atlas uses it.
	RGBA8, // four bytes for each texel
}

Tex_Sampling :: enum {
	Linear,  // the default, which is the value 0
	Nearest, // pixel art, and a sprite at an integer scale
}

@(private)
texel_size :: proc(format: Tex_Format) -> int {
	return (format == .R8) ? 1 : 4
}

////////////////////////////////
//~ fp: Quads
//
// The quad is the one primitive. Each shape above this layer is a set of
// values of this struct: a rectangle, a glyph, a sprite, a line and a panel
// with round corners. This struct holds no member that carries a meaning. A
// member such as a font or a ninepatch belongs to the draw layer, which builds
// it from these quads.

Quad :: struct {
	dst: geo.Rect,      // the place, in points, in the space of the window
	src: geo.Rect,      // the texels of tex. A nil tex makes this member unused.
	mask_src: geo.Rect, // The texels of tex whose alpha multiplies the alpha of the
	                    // quad, across dst. A rect of 0 gives no mask. The mask is on
	                    // the texture of src, so a mask cannot end a batch.
	tex: Handle,        // A nil handle gives a solid color. The renderer then uses an
	                    // internal white texture of 1x1 texels.

	colors: [geo.Corner]geo.V4,      // the RGBA of each corner, from 0 to 1. Four equal colors give a flat quad.
	corner_radii: [geo.Corner]f32,   // points. A radius of 0 gives a square corner.
	border_thickness: f32,           // points. A value of 0 fills the quad. A larger value draws an outline.
	edge_softness: f32,              // the points of fade at an edge. 0 is hard, and about 1 is smooth.

	rotation: f32, // Radians, counter-clockwise, about the center of dst. A
	               // corner radius and a border are in the local space of the
	               // quad, so they turn with the quad.

	clip: geo.Rect, // Points. The shader removes each pixel outside this rect, so a
	                // clip cannot end a batch. A rect of 0 clips nothing.
}

////////////////////////////////
//~ fp: GPU Instance Layout
//
// The attribute declarations of the vertex shader hold this same layout:
// location i reads 16 bytes at the offset i*16. A change to one of the two
// needs the same change to the other.
//
// The members are the types of the quad itself: a geo.Rect is four floats, and
// the corner arrays lay their members out in the order of geo.Corner, which is
// the order that the shader reads. The assert below holds the two sides
// together.

@(private)
Inst :: struct {
	dst: geo.Rect,              // points
	src: geo.Rect,              // texels
	clip: geo.Rect,             // Points. A quad with no clip pushes a very large rect.
	colors: [geo.Corner]geo.V4, // top left, top right, bottom left, bottom right
	radii: [geo.Corner]f32,     // top left, top right, bottom left, bottom right
	border: f32,
	softness: f32,
	rotation: f32,
	pad: f32,
	mask_src: geo.Rect, // texels. A rect of 0 gives no mask.
}
#assert(size_of(Inst) == 10 * 16)

@(private) INST_CHUNK_CAP :: 256
@(private) Inst_List :: chunk.List(Inst, INST_CHUNK_CAP)

// The longest run of quads that follow each other and that share one texture.
// The renderer draws the batches in the order that it made them, so it keeps
// the order of the pushes.
@(private)
Batch :: struct {
	next: ^Batch,
	tex_slot: int,
	insts: Inst_List,
}

////////////////////////////////
//~ fp: State

@(private) TEX_CAP :: 512

@(private)
Tex :: struct {
	id: u32,
	w: int,
	h: int,
	format: Tex_Format,
	used: bool,
}

@(private)
State :: struct {
	initialized: bool,
	program: u32,
	vao: u32,
	instance_vbo: u32,
	u_viewport: i32,
	u_tex: i32,
	u_tex_size: i32,
	u_tex_r8: i32,
	textures: [TEX_CAP]Tex,
	white_slot: int, // The white texture of 1x1 texels. A quad with a nil texture reads it.

	//- the members of one frame
	in_frame: bool,
	frame_allocator: mem.Allocator,
	fb_size_px: geo.V2,
	scale: f32,
	first_batch: ^Batch,
	last_batch: ^Batch,
}

@(private) state: State

////////////////////////////////
//~ fp: Shaders

@(private)
VS_SRC: string : `#version 410 core
layout(location = 0) in vec4 a_dst;
layout(location = 1) in vec4 a_src;
layout(location = 2) in vec4 a_clip;
layout(location = 3) in vec4 a_color_tl;
layout(location = 4) in vec4 a_color_tr;
layout(location = 5) in vec4 a_color_bl;
layout(location = 6) in vec4 a_color_br;
layout(location = 7) in vec4 a_radii;
layout(location = 8) in vec4 a_misc; // the border, the softness, the rotation and the pad
layout(location = 9) in vec4 a_mask_src;
uniform vec2 u_viewport;             // points
out vec2 v_local;                    // the position from the center of the quad, before the rotation
out vec2 v_pos;                      // the position on the screen, in points, for the clip
out vec2 v_uv;                       // texels
out vec2 v_mask_uv;                  // texels. It holds a value where v_mask_on is above 0.
out vec4 v_color;
flat out float v_mask_on;
flat out vec2 v_half_size;
flat out vec4 v_clip;
flat out vec4 v_radii;
flat out vec2 v_border_soft;
void main() {
  vec2 t = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 corner = 2.0 * t - 1.0;
  vec2 center = 0.5 * (a_dst.xy + a_dst.zw);
  vec2 half_size = 0.5 * (a_dst.zw - a_dst.xy);
  // The band of the fade is fully outside the shape. See the fragment shader.
  // The quad therefore grows by the full width of that band, so that the band
  // has fragments. Without this step a straight edge removes the band, and the
  // corners alone become smooth. The pad is 0 for a quad with a hard edge.
  float pad = 2.0 * a_misc.y;
  vec2 local = corner * (half_size + vec2(pad));
  // A corner with no rotation comes from a_dst, because a multiplication by 0
  // or by 1 and an addition of 0 are exact in floating point. It does not come
  // from center plus or minus half_size, whose rounding is different for two
  // quads that share the coordinate of an edge. That difference opens a thin
  // gap between two tiles that touch. The term with the pad is exactly 0
  // there. A soft quad does not touch another quad, so its small error causes
  // no such gap.
  vec2 pos = a_dst.xy * (1.0 - t) + a_dst.zw * t + corner * pad;
  if (a_misc.z != 0.0) {
    // y grows toward the bottom. A turn that a person sees as counter-clockwise
    // is therefore clockwise in the coordinates, so this matrix is the transpose
    // of the usual rotation matrix.
    float c = cos(a_misc.z);
    float s = sin(a_misc.z);
    pos = center + vec2(local.x * c + local.y * s, -local.x * s + local.y * c);
  }
  gl_Position = vec4(2.0 * pos.x / u_viewport.x - 1.0, 1.0 - 2.0 * pos.y / u_viewport.y, 0.0, 1.0);
  v_local = local;
  v_pos = pos;
  // The uv grows with the quad, so a texel keeps its place on the geometry. At
  // a pad of 0 this expression gives exactly t, because 0.5 plus or minus 0.5
  // is exact in floating point.
  vec2 t_uv = 0.5 + (t - 0.5) * (half_size + vec2(pad)) / max(half_size, vec2(1e-6));
  v_uv = mix(a_src.xy, a_src.zw, t_uv);
  v_mask_uv = mix(a_mask_src.xy, a_mask_src.zw, t_uv);
  v_mask_on = (a_mask_src.z > a_mask_src.x) ? 1.0 : 0.0; // a rect of 0 gives no mask
  v_color = mix(mix(a_color_tl, a_color_tr, t.x), mix(a_color_bl, a_color_br, t.x), t.y);
  v_half_size = half_size;
  v_clip = a_clip;
  v_radii = a_radii;
  v_border_soft = a_misc.xy;
}`

@(private)
FS_SRC: string : `#version 410 core
in vec2 v_local;
in vec2 v_pos;
in vec2 v_uv;
in vec2 v_mask_uv;
in vec4 v_color;
flat in float v_mask_on;
flat in vec2 v_half_size;
flat in vec4 v_clip;
flat in vec4 v_radii;
flat in vec2 v_border_soft;
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform int u_tex_r8;
out vec4 f_color;
float rounded_box_sdf(vec2 p, vec2 half_size, float r) {
  vec2 q = abs(p) - half_size + vec2(r);
  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
  // The quadrant of the fragment chooses the radius of the corner. A local y
  // below 0 is the top half.
  float r = (v_local.x < 0.0) ? ((v_local.y < 0.0) ? v_radii.x : v_radii.z)
                              : ((v_local.y < 0.0) ? v_radii.y : v_radii.w);
  float d = rounded_box_sdf(v_local, v_half_size, r);
  float soft = max(v_border_soft.y, 0.001); // a softness of 0 gives a hard edge
  // The band of the fall is fully OUTSIDE the shape, where d is from 0 to
  // 2*soft. A fragment on the boundary therefore keeps its full coverage. Two
  // quads with hard edges that touch then show no line of half alpha at the
  // edge that they share.
  float coverage = 1.0 - smoothstep(0.0, 2.0 * soft, d);
  float border = v_border_soft.x;
  if (border > 0.0) {
    coverage *= smoothstep(0.0, 2.0 * soft, d + border); // keep the band at the edge alone
  }
  if (v_pos.x < v_clip.x || v_pos.y < v_clip.y || v_pos.x > v_clip.z || v_pos.y > v_clip.w) {
    coverage = 0.0;
  }
  vec4 texel = texture(u_tex, v_uv / u_tex_size);
  if (u_tex_r8 == 1) {
    texel = vec4(1.0, 1.0, 1.0, texel.r); // An R8 texture holds a coverage. A glyph uses it.
  }
  if (v_mask_on > 0.5) {
    coverage *= texture(u_tex, v_mask_uv / u_tex_size).a;
  }
  f_color = vec4(v_color.rgb * texel.rgb, v_color.a * texel.a * coverage);
}`

////////////////////////////////
//~ fp: Init

// A GL context must be current. Call window.open and window.equip_gl first.
init :: proc() {
	//- program
	program, program_ok := gl.load_shaders_source(VS_SRC, FS_SRC)
	if !program_ok {
		compile_msg, _, link_msg, _ := gl.get_last_error_messages()
		fmt.eprintfln("shader error:\n%s%s", compile_msg, link_msg)
		panic("shader build failed")
	}
	state.program = program
	state.u_viewport = gl.GetUniformLocation(program, "u_viewport")
	state.u_tex = gl.GetUniformLocation(program, "u_tex")
	state.u_tex_size = gl.GetUniformLocation(program, "u_tex_size")
	state.u_tex_r8 = gl.GetUniformLocation(program, "u_tex_r8")

	//- The vertex array and the instance buffer. There are 10 attributes of
	//  type vec4, and each one goes with the instance. The corner of a vertex
	//  comes from gl_VertexID, so there is no data for each vertex.
	gl.GenVertexArrays(1, &state.vao)
	gl.BindVertexArray(state.vao)
	gl.GenBuffers(1, &state.instance_vbo)
	gl.BindBuffer(gl.ARRAY_BUFFER, state.instance_vbo)
	for i in u32(0) ..< 10 {
		gl.EnableVertexAttribArray(i)
		gl.VertexAttribPointer(i, 4, gl.FLOAT, false, size_of(Inst), uintptr(i * 16))
		gl.VertexAttribDivisor(i, 1)
	}

	// A row of an R8 texture does not align to 4 bytes. This value changes
	// nothing for an RGBA8 texture.
	gl.PixelStorei(gl.UNPACK_ALIGNMENT, 1)

	//- The white texture. A quad of a solid color reads it.
	white := [4]u8{255, 255, 255, 255}
	white_handle := tex_alloc(1, 1, .RGBA8, .Nearest, white[:])
	state.white_slot = int(white_handle - 1)
	state.initialized = true
}

////////////////////////////////
//~ fp: Textures

// opt_data can be nil. The texture then holds the bytes that the memory held,
// and a later call to tex_update writes it. The glyph atlas grows in that
// way. Each size is in texels.
tex_alloc :: proc(w, h: int, format: Tex_Format, sampling: Tex_Sampling, opt_data: []u8) -> Handle {
	result: Handle

	slot := TEX_CAP
	for i in 0 ..< TEX_CAP {
		if !state.textures[i].used { slot = i; break }
	}

	if slot < TEX_CAP && w > 0 && h > 0 && (format == .R8 || format == .RGBA8) {
		assert(opt_data == nil || len(opt_data) == w * h * texel_size(format))
		gl_internal: i32 = (format == .R8) ? gl.R8 : gl.RGBA8
		gl_format: u32 = (format == .R8) ? gl.RED : gl.RGBA
		gl_filter: i32 = (sampling == .Nearest) ? gl.NEAREST : gl.LINEAR

		id: u32
		gl.GenTextures(1, &id)
		gl.BindTexture(gl.TEXTURE_2D, id)
		gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl_filter)
		gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl_filter)
		gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
		gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
		gl.TexImage2D(gl.TEXTURE_2D, 0, gl_internal, i32(w), i32(h), 0, gl_format, gl.UNSIGNED_BYTE, raw_data(opt_data))

		tex := &state.textures[slot]
		tex.id = id
		tex.w = w
		tex.h = h
		tex.format = format
		tex.used = true
		result = Handle(slot + 1)
	}
	return result
}

tex_release :: proc(handle: Handle) {
	if handle != 0 && int(handle) <= TEX_CAP {
		tex := &state.textures[handle - 1]
		if tex.used {
			gl.DeleteTextures(1, &tex.id)
			tex^ = {}
		}
	}
}

tex_update :: proc(handle: Handle, x, y, w, h: int, data: []u8) {
	if handle != 0 && int(handle) <= TEX_CAP && data != nil {
		tex := &state.textures[handle - 1]
		if tex.used && x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= tex.w && y + h <= tex.h {
			assert(len(data) == w * h * texel_size(tex.format))
			gl_format: u32 = (tex.format == .R8) ? gl.RED : gl.RGBA
			gl.BindTexture(gl.TEXTURE_2D, tex.id)
			gl.TexSubImage2D(gl.TEXTURE_2D, 0, i32(x), i32(y), i32(w), i32(h), gl_format, gl.UNSIGNED_BYTE, raw_data(data))
		}
	}
}

////////////////////////////////
//~ fp: Frame

// frame_allocator holds each quad of this frame. The caller frees that
// allocator after frame_end, as it frees each allocator of a frame.
frame_begin :: proc(frame_allocator: mem.Allocator, framebuffer_size_px: geo.V2, points_to_pixels: f32) {
	state.in_frame = true
	state.frame_allocator = frame_allocator
	state.fb_size_px = framebuffer_size_px
	assert(points_to_pixels > 0) // The draw layer turns a ZII 0 into 1. This layer needs a value above 0.
	state.scale = points_to_pixels
	state.first_batch = nil
	state.last_batch = nil

	gl.Viewport(0, 0, i32(framebuffer_size_px.x), i32(framebuffer_size_px.y))
	gl.ClearColor(0.0, 0.0, 0.0, 1.0)
	gl.Clear(gl.COLOR_BUFFER_BIT)
}

push_quad :: proc(quad: ^Quad) {
	if !state.in_frame { return }

	//- Find the texture. A nil handle, and a handle to a texture that is gone,
	//  both give the white texture, which draws a solid color.
	slot := state.white_slot
	if quad.tex != 0 && int(quad.tex) <= TEX_CAP && state.textures[quad.tex - 1].used {
		slot = int(quad.tex - 1)
	}

	//- Add to the current batch. A change of texture starts a new batch.
	batch := state.last_batch
	if batch == nil || batch.tex_slot != slot {
		batch = new(Batch, state.frame_allocator)
		batch.tex_slot = slot
		if state.first_batch == nil {
			state.first_batch = batch
			state.last_batch = batch
		} else {
			state.last_batch.next = batch
			state.last_batch = batch
		}
	}

	//- Write the quad in the layout that the GPU reads.
	inst := chunk.push(&batch.insts, Inst{}, state.frame_allocator)
	inst.dst = quad.dst
	inst.src = quad.src
	if quad.clip.max.x > quad.clip.min.x && quad.clip.max.y > quad.clip.min.y {
		inst.clip = quad.clip
	} else {
		// A clip of 0 clips nothing. Put a rect here that holds each fragment.
		inst.clip = {{-1e9, -1e9}, {1e9, 1e9}}
	}
	inst.colors = quad.colors
	inst.radii = quad.corner_radii
	inst.border = quad.border_thickness
	inst.softness = quad.edge_softness
	inst.rotation = quad.rotation
	inst.mask_src = quad.mask_src
}

// send the batches of push_quad to the GPU, and draw them
frame_end :: proc() {
	if !state.in_frame { return }

	gl.Enable(gl.BLEND)
	gl.BlendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
	gl.Disable(gl.DEPTH_TEST)
	gl.Disable(gl.CULL_FACE)
	gl.UseProgram(state.program)
	gl.BindVertexArray(state.vao)
	gl.BindBuffer(gl.ARRAY_BUFFER, state.instance_vbo)
	gl.ActiveTexture(gl.TEXTURE0)
	gl.Uniform1i(state.u_tex, 0)
	gl.Uniform2f(state.u_viewport,
	             state.fb_size_px.x / state.scale,
	             state.fb_size_px.y / state.scale)

	for batch := state.first_batch; batch != nil; batch = batch.next {
		tex := &state.textures[batch.tex_slot]
		gl.BindTexture(gl.TEXTURE_2D, tex.id)
		gl.Uniform2f(state.u_tex_size, f32(tex.w), f32(tex.h))
		gl.Uniform1i(state.u_tex_r8, (tex.format == .R8) ? 1 : 0)

		// Discard the contents of the buffer, then write the new contents. Each
		// batch draws from the offset 0 of the buffer, because macOS gives GL 4.1
		// at most, and GL 4.1 has no draw call with a base instance.
		total_size := batch.insts.count * size_of(Inst)
		gl.BufferData(gl.ARRAY_BUFFER, total_size, nil, gl.STREAM_DRAW)
		offset := 0
		for c := batch.insts.first; c != nil; c = c.next {
			chunk_size := c.count * size_of(Inst)
			gl.BufferSubData(gl.ARRAY_BUFFER, offset, chunk_size, raw_data(c.items[:]))
			offset += chunk_size
		}
		gl.DrawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, i32(batch.insts.count))
	}

	when ODIN_DEBUG {
		err := gl.GetError()
		if err != 0 { fmt.eprintfln("gl error: 0x%x", err) }
	}

	state.in_frame = false
	state.frame_allocator = {}
	state.first_batch = nil
	state.last_batch = nil
}
