#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "base/print.h"
#include "base/os.h"
#include "gfx/render.h"

////////////////////////////////
//~ fp: OpenGL Backend
//
// One shader, one instanced draw call per batch. Every quad is 4 vertices
// synthesized from gl_VertexID; all actual data rides per-instance. The
// fragment shader does the rest: rounded-box SDF for corners and borders,
// softness as a smoothstep band, per-quad clip, texture sampling with the
// R8-as-coverage special case.
//
// No GL type or call escapes this file.

#if OS_MAC
# define GL_SILENCE_DEPRECATION // deliberate GL-over-Metal choice; see cocoa.m
# include <OpenGL/gl3.h>
# define R_GL_BACKEND 1
#elif OS_WINDOWS
# include "gfx/win/gl.c" // the 1.1 header + runtime loading of everything newer
# define R_GL_BACKEND 1
#else
# define R_GL_BACKEND 0
#endif

#if R_GL_BACKEND

////////////////////////////////
//~ fp: GPU Instance Layout
//
// Mirrored by the vertex shader's attribute declarations: location i reads
// 16 bytes at offset i*16. Keep the two in lockstep.

typedef struct {
  F32 dst[4];       // min.x min.y max.x max.y, points
  F32 src[4];       // texel rect
  F32 clip[4];      // points; pushed as a huge rect when the quad has none
  F32 colors[4][4]; // TL TR BL BR
  F32 radii[4];     // TL TR BL BR
  F32 border;
  F32 softness;
  F32 rotation;
  F32 pad;
  F32 mask_src[4];  // texel rect; all zeros = no mask
} R_GL_Inst;
StaticAssert(sizeof(R_GL_Inst) == 10 * 16, r_gl_inst_matches_attrib_layout);

#define R_GL_INST_CHUNK_CAP 256
typedef struct R_GL_InstChunk R_GL_InstChunk;
struct R_GL_InstChunk {
  R_GL_InstChunk* next;
  U64 count;
  R_GL_Inst insts[R_GL_INST_CHUNK_CAP];
};

// A maximal run of consecutive quads sharing a texture; drawn in formation
// order, so push order is preserved exactly.
typedef struct R_GL_Batch R_GL_Batch;
struct R_GL_Batch {
  R_GL_Batch* next;
  U32 tex_slot;
  U64 inst_count;
  R_GL_InstChunk* first_chunk;
  R_GL_InstChunk* last_chunk;
};

////////////////////////////////
//~ fp: State

#define R_GL_TEX_CAP 512

typedef struct {
  GLuint id;
  I32 w;
  I32 h;
  R_TexFormat format;
  B32 used;
} R_GL_Tex;

typedef struct {
  B32 initialized;
  GLuint program;
  GLuint vao;
  GLuint instance_vbo;
  GLint u_viewport;
  GLint u_tex;
  GLint u_tex_size;
  GLint u_tex_r8;
  R_GL_Tex textures[R_GL_TEX_CAP];
  U32 white_slot; // 1x1 white texture; what nil-tex quads resolve to

  //- per-frame
  Arena* frame_arena;
  V2 fb_size_px;
  F32 scale;
  R_GL_Batch* first_batch;
  R_GL_Batch* last_batch;
} R_GL_State;

global R_GL_State r_gl_state;

////////////////////////////////
//~ fp: Shaders

global const char* r_gl__vs_src =
  "#version 410 core\n"
  "layout(location = 0) in vec4 a_dst;\n"
  "layout(location = 1) in vec4 a_src;\n"
  "layout(location = 2) in vec4 a_clip;\n"
  "layout(location = 3) in vec4 a_color_tl;\n"
  "layout(location = 4) in vec4 a_color_tr;\n"
  "layout(location = 5) in vec4 a_color_bl;\n"
  "layout(location = 6) in vec4 a_color_br;\n"
  "layout(location = 7) in vec4 a_radii;\n"
  "layout(location = 8) in vec4 a_misc;\n" // border, softness, rotation, pad
  "layout(location = 9) in vec4 a_mask_src;\n"
  "uniform vec2 u_viewport;\n"             // points
  "out vec2 v_local;\n"                    // position relative to quad center, unrotated
  "out vec2 v_pos;\n"                      // screen position, points (for clip)
  "out vec2 v_uv;\n"                       // texels
  "out vec2 v_mask_uv;\n"                  // texels; meaningful when v_mask_on
  "out vec4 v_color;\n"
  "flat out float v_mask_on;\n"
  "flat out vec2 v_half_size;\n"
  "flat out vec4 v_clip;\n"
  "flat out vec4 v_radii;\n"
  "flat out vec2 v_border_soft;\n"
  "void main() {\n"
  "  vec2 t = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
  "  vec2 corner = 2.0 * t - 1.0;\n"
  "  vec2 center = 0.5 * (a_dst.xy + a_dst.zw);\n"
  "  vec2 half_size = 0.5 * (a_dst.zw - a_dst.xy);\n"
  // the fade band lies entirely outside the shape (see the fragment shader),
  // so the quad inflates by the band's full width to give it fragments to
  // live in -- without this, straight edges clip the band away and only
  // corners come out antialiased. pad is 0 for hard-edged quads.
  "  float pad = 2.0 * a_misc.y;\n"
  "  vec2 local = corner * (half_size + vec2(pad));\n"
  // unrotated corners come straight from a_dst (multiplying by 0/1 and adding
  // 0 are exact in fp), NOT from center +- half_size, whose rounding differs
  // between two quads sharing an edge coordinate and opens sliver gaps
  // between abutting tiles; the +- pad term is exactly 0 there (soft quads
  // don't abut, so their inexactness is fine)
  "  vec2 pos = a_dst.xy * (1.0 - t) + a_dst.zw * t + corner * pad;\n"
  "  if (a_misc.z != 0.0) {\n"
  // y grows downward, so visual counter-clockwise is a clockwise rotation
  // in coordinate terms -- hence the transposed rotation matrix
  "    float c = cos(a_misc.z);\n"
  "    float s = sin(a_misc.z);\n"
  "    pos = center + vec2(local.x * c + local.y * s, -local.x * s + local.y * c);\n"
  "  }\n"
  "  gl_Position = vec4(2.0 * pos.x / u_viewport.x - 1.0, 1.0 - 2.0 * pos.y / u_viewport.y, 0.0, 1.0);\n"
  "  v_local = local;\n"
  "  v_pos = pos;\n"
  // uv stretches with the inflation so texels stay glued to the geometry;
  // at pad 0 this reduces to exactly t (0.5 +- 0.5 is exact in fp)
  "  vec2 t_uv = 0.5 + (t - 0.5) * (half_size + vec2(pad)) / max(half_size, vec2(1e-6));\n"
  "  v_uv = mix(a_src.xy, a_src.zw, t_uv);\n"
  "  v_mask_uv = mix(a_mask_src.xy, a_mask_src.zw, t_uv);\n"
  "  v_mask_on = (a_mask_src.z > a_mask_src.x) ? 1.0 : 0.0;\n" // zero rect = no mask
  "  v_color = mix(mix(a_color_tl, a_color_tr, t.x), mix(a_color_bl, a_color_br, t.x), t.y);\n"
  "  v_half_size = half_size;\n"
  "  v_clip = a_clip;\n"
  "  v_radii = a_radii;\n"
  "  v_border_soft = a_misc.xy;\n"
  "}\n";

global const char* r_gl__fs_src =
  "#version 410 core\n"
  "in vec2 v_local;\n"
  "in vec2 v_pos;\n"
  "in vec2 v_uv;\n"
  "in vec2 v_mask_uv;\n"
  "in vec4 v_color;\n"
  "flat in float v_mask_on;\n"
  "flat in vec2 v_half_size;\n"
  "flat in vec4 v_clip;\n"
  "flat in vec4 v_radii;\n"
  "flat in vec2 v_border_soft;\n"
  "uniform sampler2D u_tex;\n"
  "uniform vec2 u_tex_size;\n"
  "uniform int u_tex_r8;\n"
  "out vec4 f_color;\n"
  "float rounded_box_sdf(vec2 p, vec2 half_size, float r) {\n"
  "  vec2 q = abs(p) - half_size + vec2(r);\n"
  "  return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
  "}\n"
  "void main() {\n"
  // quadrant picks the corner radius: local y < 0 is the top half
  "  float r = (v_local.x < 0.0) ? ((v_local.y < 0.0) ? v_radii.x : v_radii.z)\n"
  "                              : ((v_local.y < 0.0) ? v_radii.y : v_radii.w);\n"
  "  float d = rounded_box_sdf(v_local, v_half_size, r);\n"
  "  float soft = max(v_border_soft.y, 0.001);\n" // 0 softness reads as hard edge
  // the falloff band sits entirely OUTSIDE the shape (d in [0, 2*soft]), so a
  // fragment on the boundary keeps full coverage -- abutting hard-edged quads
  // tile without half-alpha seam lines at shared edges
  "  float coverage = 1.0 - smoothstep(0.0, 2.0 * soft, d);\n"
  "  float border = v_border_soft.x;\n"
  "  if (border > 0.0) {\n"
  "    coverage *= smoothstep(0.0, 2.0 * soft, d + border);\n" // keep only the edge band
  "  }\n"
  "  if (v_pos.x < v_clip.x || v_pos.y < v_clip.y || v_pos.x > v_clip.z || v_pos.y > v_clip.w) {\n"
  "    coverage = 0.0;\n"
  "  }\n"
  "  vec4 texel = texture(u_tex, v_uv / u_tex_size);\n"
  "  if (u_tex_r8 == 1) {\n"
  "    texel = vec4(1.0, 1.0, 1.0, texel.r);\n" // R8 is coverage (glyphs)
  "  }\n"
  "  if (v_mask_on > 0.5) {\n"
  "    coverage *= texture(u_tex, v_mask_uv / u_tex_size).a;\n"
  "  }\n"
  "  f_color = vec4(v_color.rgb * texel.rgb, v_color.a * texel.a * coverage);\n"
  "}\n";

internal GLuint r_gl__compile_shader(GLenum kind, const char* src) {
  GLuint shader = glCreateShader(kind);
  glShaderSource(shader, 1, &src, 0);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if(!ok) {
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(log), 0, log);
    printf_str8("shader compile error:\n%s\n", log);
    AssertAlways(!"shader compile failed");
  }
  return shader;
}

////////////////////////////////
//~ fp: Init

// Requires a current GL context: wnd_open + wnd_equip_gl first.
internal void r_init(void) {
#if OS_WINDOWS
  r_gl__load_procs(); // needs that current context too -- see win/gl.c
#endif

  //- program
  GLuint vs = r_gl__compile_shader(GL_VERTEX_SHADER, r_gl__vs_src);
  GLuint fs = r_gl__compile_shader(GL_FRAGMENT_SHADER, r_gl__fs_src);
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  {
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if(!ok) {
      char log[1024] = {0};
      glGetProgramInfoLog(program, sizeof(log), 0, log);
      printf_str8("shader link error:\n%s\n", log);
      AssertAlways(!"shader link failed");
    }
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  r_gl_state.program = program;
  r_gl_state.u_viewport = glGetUniformLocation(program, "u_viewport");
  r_gl_state.u_tex      = glGetUniformLocation(program, "u_tex");
  r_gl_state.u_tex_size = glGetUniformLocation(program, "u_tex_size");
  r_gl_state.u_tex_r8   = glGetUniformLocation(program, "u_tex_r8");

  //- vao + instance buffer: 10 vec4 attributes, all per-instance; the
  //  vertex corner comes from gl_VertexID, so there is no per-vertex data
  glGenVertexArrays(1, &r_gl_state.vao);
  glBindVertexArray(r_gl_state.vao);
  glGenBuffers(1, &r_gl_state.instance_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, r_gl_state.instance_vbo);
  for(U32 i = 0; i < 10; i += 1) {
    glEnableVertexAttribArray(i);
    glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(R_GL_Inst), (void*)(U64)(i * 16));
    glVertexAttribDivisor(i, 1);
  }

  // R8 rows are not 4-byte aligned; harmless for RGBA8
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  //- the white texture: what solid-color quads sample
  U32 white = 0xFFFFFFFF;
  R_Handle white_handle = r_tex_alloc(1, 1, R_TexFormat_RGBA8, R_TexSampling_Nearest, &white);
  r_gl_state.white_slot = (U32)(white_handle.u64 - 1);
  r_gl_state.initialized = 1;
}

////////////////////////////////
//~ fp: Textures

internal R_Handle r_tex_alloc(I32 w, I32 h, R_TexFormat format, R_TexSampling sampling, void* opt_data) {
  R_Handle result = {0};

  U32 slot = R_GL_TEX_CAP;
  for(U32 i = 0; i < R_GL_TEX_CAP; i += 1) {
    if(!r_gl_state.textures[i].used) { slot = i; break; }
  }

  if(slot < R_GL_TEX_CAP && w > 0 && h > 0 &&
     (format == R_TexFormat_R8 || format == R_TexFormat_RGBA8)) {
    GLenum gl_internal = (format == R_TexFormat_R8) ? GL_R8 : GL_RGBA8;
    GLenum gl_format   = (format == R_TexFormat_R8) ? GL_RED : GL_RGBA;
    GLint  gl_filter   = (sampling == R_TexSampling_Nearest) ? GL_NEAREST : GL_LINEAR;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)gl_internal, w, h, 0, gl_format, GL_UNSIGNED_BYTE, opt_data);

    R_GL_Tex* tex = &r_gl_state.textures[slot];
    tex->id = id;
    tex->w = w;
    tex->h = h;
    tex->format = format;
    tex->used = 1;
    result.u64 = slot + 1;
  }
  return result;
}

internal void r_tex_release(R_Handle handle) {
  if(handle.u64 != 0 && handle.u64 <= R_GL_TEX_CAP) {
    R_GL_Tex* tex = &r_gl_state.textures[handle.u64 - 1];
    if(tex->used) {
      glDeleteTextures(1, &tex->id);
      MemoryZeroStruct(tex);
    }
  }
}

internal void r_tex_update(R_Handle handle, I32 x, I32 y, I32 w, I32 h, void* data) {
  if(handle.u64 != 0 && handle.u64 <= R_GL_TEX_CAP && data != 0) {
    R_GL_Tex* tex = &r_gl_state.textures[handle.u64 - 1];
    if(tex->used && x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= tex->w && y + h <= tex->h) {
      GLenum gl_format = (tex->format == R_TexFormat_R8) ? GL_RED : GL_RGBA;
      glBindTexture(GL_TEXTURE_2D, tex->id);
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, gl_format, GL_UNSIGNED_BYTE, data);
    }
  }
}

////////////////////////////////
//~ fp: Frame

internal void r_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels) {
  r_gl_state.frame_arena = frame_arena;
  r_gl_state.fb_size_px = framebuffer_size_px;
  Assert(points_to_pixels > 0); // draw owns the ZII 0->1 default; here it's contract
  r_gl_state.scale = points_to_pixels;
  r_gl_state.first_batch = 0;
  r_gl_state.last_batch = 0;

  glViewport(0, 0, (GLsizei)framebuffer_size_px.x, (GLsizei)framebuffer_size_px.y);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

internal void r_push_quad(R_Quad* quad) {
  Arena* arena = r_gl_state.frame_arena;
  if(arena == 0) { return; }

  //- resolve texture: nil (or dangling) draws as solid color via white
  U32 slot = r_gl_state.white_slot;
  if(quad->tex.u64 != 0 && quad->tex.u64 <= R_GL_TEX_CAP &&
     r_gl_state.textures[quad->tex.u64 - 1].used) {
    slot = (U32)(quad->tex.u64 - 1);
  }

  //- extend the current run, or start a new one on texture change
  R_GL_Batch* batch = r_gl_state.last_batch;
  if(batch == 0 || batch->tex_slot != slot) {
    batch = push_array(arena, R_GL_Batch, 1);
    batch->tex_slot = slot;
    SLLQueuePush(r_gl_state.first_batch, r_gl_state.last_batch, batch);
  }
  R_GL_InstChunk* chunk = batch->last_chunk;
  if(chunk == 0 || chunk->count >= R_GL_INST_CHUNK_CAP) {
    chunk = push_array(arena, R_GL_InstChunk, 1);
    SLLQueuePush(batch->first_chunk, batch->last_chunk, chunk);
  }

  //- flatten the quad into the GPU layout
  R_GL_Inst* inst = &chunk->insts[chunk->count];
  chunk->count += 1;
  batch->inst_count += 1;

  inst->dst[0] = quad->dst.min.x;
  inst->dst[1] = quad->dst.min.y;
  inst->dst[2] = quad->dst.max.x;
  inst->dst[3] = quad->dst.max.y;
  inst->src[0] = quad->src.min.x;
  inst->src[1] = quad->src.min.y;
  inst->src[2] = quad->src.max.x;
  inst->src[3] = quad->src.max.y;
  if(quad->clip.max.x > quad->clip.min.x && quad->clip.max.y > quad->clip.min.y) {
    inst->clip[0] = quad->clip.min.x;
    inst->clip[1] = quad->clip.min.y;
    inst->clip[2] = quad->clip.max.x;
    inst->clip[3] = quad->clip.max.y;
  } else {
    // zero clip = no clip: substitute a rect nothing escapes
    inst->clip[0] = -1e9f;
    inst->clip[1] = -1e9f;
    inst->clip[2] = 1e9f;
    inst->clip[3] = 1e9f;
  }
  for(U32 corner = 0; corner < Corner_COUNT; corner += 1) {
    inst->colors[corner][0] = quad->colors[corner].x;
    inst->colors[corner][1] = quad->colors[corner].y;
    inst->colors[corner][2] = quad->colors[corner].z;
    inst->colors[corner][3] = quad->colors[corner].w;
    inst->radii[corner] = quad->corner_radii[corner];
  }
  inst->border = quad->border_thickness;
  inst->softness = quad->edge_softness;
  inst->rotation = quad->rotation;
  inst->pad = 0;
  inst->mask_src[0] = quad->mask_src.min.x;
  inst->mask_src[1] = quad->mask_src.min.y;
  inst->mask_src[2] = quad->mask_src.max.x;
  inst->mask_src[3] = quad->mask_src.max.y;
}

internal void r_frame_end(void) {
  if(r_gl_state.frame_arena == 0) { return; }

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(r_gl_state.program);
  glBindVertexArray(r_gl_state.vao);
  glBindBuffer(GL_ARRAY_BUFFER, r_gl_state.instance_vbo);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(r_gl_state.u_tex, 0);
  glUniform2f(r_gl_state.u_viewport,
              r_gl_state.fb_size_px.x / r_gl_state.scale,
              r_gl_state.fb_size_px.y / r_gl_state.scale);

  for(R_GL_Batch* batch = r_gl_state.first_batch; batch != 0; batch = batch->next) {
    R_GL_Tex* tex = &r_gl_state.textures[batch->tex_slot];
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glUniform2f(r_gl_state.u_tex_size, (F32)tex->w, (F32)tex->h);
    glUniform1i(r_gl_state.u_tex_r8, tex->format == R_TexFormat_R8);

    // orphan-and-refill streaming: macOS caps at GL 4.1, which lacks
    // BaseInstance draws, so every batch draws from offset 0 of the vbo
    U64 total_size = batch->inst_count * sizeof(R_GL_Inst);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)total_size, 0, GL_STREAM_DRAW);
    U64 offset = 0;
    for(R_GL_InstChunk* chunk = batch->first_chunk; chunk != 0; chunk = chunk->next) {
      U64 chunk_size = chunk->count * sizeof(R_GL_Inst);
      glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)chunk_size, chunk->insts);
      offset += chunk_size;
    }
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)batch->inst_count);
  }

#if BUILD_DEBUG
  {
    GLenum err = glGetError();
    if(err != 0) { printf_str8("gl error: 0x%x\n", err); }
  }
#endif

  r_gl_state.frame_arena = 0;
  r_gl_state.first_batch = 0;
  r_gl_state.last_batch = 0;
}

#else // !R_GL_BACKEND

////////////////////////////////
//~ fp: Stubs
//
// GL headers + function loading for this OS still to come; stubs keep the
// build compiling (mirrors the window layer's GLX stubs).

internal void r_init(void) { NotImplemented; }
internal R_Handle r_tex_alloc(I32 w, I32 h, R_TexFormat format, R_TexSampling sampling, void* opt_data) {
  Unused(w); Unused(h); Unused(format); Unused(sampling); Unused(opt_data);
  NotImplemented;
  R_Handle result = {0};
  return result;
}
internal void r_tex_release(R_Handle handle) { Unused(handle); NotImplemented; }
internal void r_tex_update(R_Handle handle, I32 x, I32 y, I32 w, I32 h, void* data) {
  Unused(handle); Unused(x); Unused(y); Unused(w); Unused(h); Unused(data);
  NotImplemented;
}
internal void r_frame_begin(Arena* frame_arena, V2 framebuffer_size_px, F32 points_to_pixels) {
  Unused(frame_arena); Unused(framebuffer_size_px); Unused(points_to_pixels);
  NotImplemented;
}
internal void r_push_quad(R_Quad* quad) { Unused(quad); NotImplemented; }
internal void r_frame_end(void) { NotImplemented; }

#endif // R_GL_BACKEND
