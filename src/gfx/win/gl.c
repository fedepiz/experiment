#include "base/core.h"

////////////////////////////////
//~ fp: Windows GL Loading
//
// opengl32.dll holds GL 1.1 and nothing later. Each later entry point is in
// the driver, and this file reads it by name while the program runs. Khronos
// supplies glext.h for that purpose. This program uses a small part of GL, so
// the declarations below are in this file. The wgl constants in win/window.c
// follow the same rule.
//
// No name below hides another name. The <GL/gl.h> of Microsoft stops at 1.1,
// so each name below is new to this translation unit. Each pointer is a global
// with the name of its GL function, so the code in render.c, which each
// platform shares, needs no change. r_init calls the loader after
// wnd_equip_gl makes a context current, because wglGetProcAddress answers for
// the current context, and gives nothing without one.

// The OS_WINDOWS test makes this file empty for clangd, which parses it alone
// on each platform, and which has no windows.h on another platform. A real
// build includes this file on Windows only, through gfx/render.c.
#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

//- fp: the types and the enums that the header of GL 1.1 does not hold
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

//- fp: the entry points that come from the driver. Each signature follows the
//  specification of GL 4.1.
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
  // A null pointer here shows that the GL of the driver is older than 4.1
  // core. There is no other path, so the program stops.
#define X(name) \
  name = (Glue(PFN_, name))(void*)wglGetProcAddress(#name); \
  AssertAlways(name != 0);
  R_GL_PROC_LIST
#undef X
}

#endif // OS_WINDOWS
