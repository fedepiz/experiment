#include "base/core.h"

////////////////////////////////
//~ fp: Windows GL Loading
//
// opengl32.dll speaks GL 1.1 and nothing newer; every entry point past that
// lives in the driver and is fetched by name at runtime. Khronos ships
// glext.h for this, but the slice we use is small enough to spell inline --
// the same choice as the wgl constants in win/window.c.
//
// Nothing here shadows anything: MS <GL/gl.h> stops at 1.1, so every name
// below is new to the TU. The pointers are globals named exactly like the GL
// functions, so the shared render.c code compiles unchanged; r_init calls the
// loader once wnd_equip_gl has made a context current (wglGetProcAddress
// answers per-context, and nothing without one).

// The OS_WINDOWS guard keeps the file quiet in clangd, which parses it
// standalone on every platform (no windows.h off Windows); real builds only
// include it on Windows anyway, via gfx/render.c.
#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

//- fp: types and enums the 1.1 header lacks
typedef char     GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;

#define GL_CLAMP_TO_EDGE   0x812F
#define GL_R8              0x8229
#define GL_TEXTURE0        0x84C0
#define GL_ARRAY_BUFFER    0x8892
#define GL_STREAM_DRAW     0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82

//- fp: driver-loaded entry points (signatures per the GL 4.1 spec)
typedef GLuint (APIENTRY* PFN_glCreateShader)(GLenum type);
typedef void   (APIENTRY* PFN_glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void   (APIENTRY* PFN_glCompileShader)(GLuint shader);
typedef void   (APIENTRY* PFN_glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
typedef void   (APIENTRY* PFN_glGetShaderInfoLog)(GLuint shader, GLsizei buf_size, GLsizei* length, GLchar* info_log);
typedef GLuint (APIENTRY* PFN_glCreateProgram)(void);
typedef void   (APIENTRY* PFN_glAttachShader)(GLuint program, GLuint shader);
typedef void   (APIENTRY* PFN_glLinkProgram)(GLuint program);
typedef void   (APIENTRY* PFN_glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
typedef void   (APIENTRY* PFN_glGetProgramInfoLog)(GLuint program, GLsizei buf_size, GLsizei* length, GLchar* info_log);
typedef void   (APIENTRY* PFN_glDeleteShader)(GLuint shader);
typedef void   (APIENTRY* PFN_glUseProgram)(GLuint program);
typedef GLint  (APIENTRY* PFN_glGetUniformLocation)(GLuint program, const GLchar* name);
typedef void   (APIENTRY* PFN_glUniform1i)(GLint location, GLint v0);
typedef void   (APIENTRY* PFN_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
typedef void   (APIENTRY* PFN_glGenVertexArrays)(GLsizei n, GLuint* arrays);
typedef void   (APIENTRY* PFN_glBindVertexArray)(GLuint array);
typedef void   (APIENTRY* PFN_glGenBuffers)(GLsizei n, GLuint* buffers);
typedef void   (APIENTRY* PFN_glBindBuffer)(GLenum target, GLuint buffer);
typedef void   (APIENTRY* PFN_glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void   (APIENTRY* PFN_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
typedef void   (APIENTRY* PFN_glEnableVertexAttribArray)(GLuint index);
typedef void   (APIENTRY* PFN_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void   (APIENTRY* PFN_glVertexAttribDivisor)(GLuint index, GLuint divisor);
typedef void   (APIENTRY* PFN_glDrawArraysInstanced)(GLenum mode, GLint first, GLsizei count, GLsizei instance_count);
typedef void   (APIENTRY* PFN_glActiveTexture)(GLenum texture);

#define R_GL_PROC_LIST \
  X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) \
  X(glGetShaderInfoLog) X(glCreateProgram) X(glAttachShader) X(glLinkProgram) \
  X(glGetProgramiv) X(glGetProgramInfoLog) X(glDeleteShader) X(glUseProgram) \
  X(glGetUniformLocation) X(glUniform1i) X(glUniform2f) X(glGenVertexArrays) \
  X(glBindVertexArray) X(glGenBuffers) X(glBindBuffer) X(glBufferData) \
  X(glBufferSubData) X(glEnableVertexAttribArray) X(glVertexAttribPointer) \
  X(glVertexAttribDivisor) X(glDrawArraysInstanced) X(glActiveTexture)

#define X(name) global Glue(PFN_, name) name = 0;
R_GL_PROC_LIST
#undef X

internal void r_gl__load_procs(void) {
  // any null here means the driver's GL is older than 4.1 core -- no
  // sensible fallback exists, so stop
#define X(name) \
  name = (Glue(PFN_, name))(void*)wglGetProcAddress(#name); \
  AssertAlways(name != 0);
  R_GL_PROC_LIST
#undef X
}

#endif // OS_WINDOWS
