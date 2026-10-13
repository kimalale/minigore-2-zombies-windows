#include "gl_wrap.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CDECL
#undef CDECL
#endif
#define CDECL __attribute__((cdecl))

/* Function pointer types for OpenGL 2.0 / extensions */
typedef void (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_glBindAttribLocation)(GLuint, GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBlendEquationSeparate)(GLenum, GLenum);
typedef void (APIENTRY *PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (APIENTRY *PFN_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDisableVertexAttribArray)(GLuint);
typedef void (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glGetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
typedef void (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef GLint (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void (APIENTRY *PFN_glUniform1fv)(GLint, GLsizei, const GLfloat *);
typedef void (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRY *PFN_glUniform1iv)(GLint, GLsizei, const GLint *);
typedef void (APIENTRY *PFN_glUniform2fv)(GLint, GLsizei, const GLfloat *);
typedef void (APIENTRY *PFN_glUniform2iv)(GLint, GLsizei, const GLint *);
typedef void (APIENTRY *PFN_glUniform3fv)(GLint, GLsizei, const GLfloat *);
typedef void (APIENTRY *PFN_glUniform3iv)(GLint, GLsizei, const GLint *);
typedef void (APIENTRY *PFN_glUniform4fv)(GLint, GLsizei, const GLfloat *);
typedef void (APIENTRY *PFN_glUniform4iv)(GLint, GLsizei, const GLint *);
typedef void (APIENTRY *PFN_glUniformMatrix2fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (APIENTRY *PFN_glUniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (APIENTRY *PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);

/* Static function pointers */
static PFN_glActiveTexture pfn_glActiveTexture = NULL;
static PFN_glAttachShader pfn_glAttachShader = NULL;
static PFN_glBindAttribLocation pfn_glBindAttribLocation = NULL;
static PFN_glBindBuffer pfn_glBindBuffer = NULL;
static PFN_glBindFramebuffer pfn_glBindFramebuffer = NULL;
static PFN_glBlendEquationSeparate pfn_glBlendEquationSeparate = NULL;
static PFN_glBlendFuncSeparate pfn_glBlendFuncSeparate = NULL;
static PFN_glBufferData pfn_glBufferData = NULL;
static PFN_glBufferSubData pfn_glBufferSubData = NULL;
static PFN_glCheckFramebufferStatus pfn_glCheckFramebufferStatus = NULL;
static PFN_glCompileShader pfn_glCompileShader = NULL;
static PFN_glCompressedTexImage2D pfn_glCompressedTexImage2D = NULL;
static PFN_glCreateProgram pfn_glCreateProgram = NULL;
static PFN_glCreateShader pfn_glCreateShader = NULL;
static PFN_glDeleteBuffers pfn_glDeleteBuffers = NULL;
static PFN_glDeleteFramebuffers pfn_glDeleteFramebuffers = NULL;
static PFN_glDisableVertexAttribArray pfn_glDisableVertexAttribArray = NULL;
static PFN_glEnableVertexAttribArray pfn_glEnableVertexAttribArray = NULL;
static PFN_glFramebufferTexture2D pfn_glFramebufferTexture2D = NULL;
static PFN_glGenBuffers pfn_glGenBuffers = NULL;
static PFN_glGenFramebuffers pfn_glGenFramebuffers = NULL;
static PFN_glGetActiveUniform pfn_glGetActiveUniform = NULL;
static PFN_glGetProgramInfoLog pfn_glGetProgramInfoLog = NULL;
static PFN_glGetProgramiv pfn_glGetProgramiv = NULL;
static PFN_glGetShaderInfoLog pfn_glGetShaderInfoLog = NULL;
static PFN_glGetShaderiv pfn_glGetShaderiv = NULL;
static PFN_glGetUniformLocation pfn_glGetUniformLocation = NULL;
static PFN_glLinkProgram pfn_glLinkProgram = NULL;
static PFN_glShaderSource pfn_glShaderSource = NULL;
static PFN_glUniform1f pfn_glUniform1f = NULL;
static PFN_glUniform1fv pfn_glUniform1fv = NULL;
static PFN_glUniform1i pfn_glUniform1i = NULL;
static PFN_glUniform1iv pfn_glUniform1iv = NULL;
static PFN_glUniform2fv pfn_glUniform2fv = NULL;
static PFN_glUniform2iv pfn_glUniform2iv = NULL;
static PFN_glUniform3fv pfn_glUniform3fv = NULL;
static PFN_glUniform3iv pfn_glUniform3iv = NULL;
static PFN_glUniform4fv pfn_glUniform4fv = NULL;
static PFN_glUniform4iv pfn_glUniform4iv = NULL;
static PFN_glUniformMatrix2fv pfn_glUniformMatrix2fv = NULL;
static PFN_glUniformMatrix3fv pfn_glUniformMatrix3fv = NULL;
static PFN_glUniformMatrix4fv pfn_glUniformMatrix4fv = NULL;
static PFN_glUseProgram pfn_glUseProgram = NULL;
static PFN_glVertexAttribPointer pfn_glVertexAttribPointer = NULL;

void gl_wrap_init(void) {
    pfn_glActiveTexture = (PFN_glActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
    pfn_glAttachShader = (PFN_glAttachShader)SDL_GL_GetProcAddress("glAttachShader");
    pfn_glBindAttribLocation = (PFN_glBindAttribLocation)SDL_GL_GetProcAddress("glBindAttribLocation");
    pfn_glBindBuffer = (PFN_glBindBuffer)SDL_GL_GetProcAddress("glBindBuffer");
    pfn_glBindFramebuffer = (PFN_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    if (!pfn_glBindFramebuffer) pfn_glBindFramebuffer = (PFN_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebufferEXT");
    pfn_glBlendEquationSeparate = (PFN_glBlendEquationSeparate)SDL_GL_GetProcAddress("glBlendEquationSeparate");
    pfn_glBlendFuncSeparate = (PFN_glBlendFuncSeparate)SDL_GL_GetProcAddress("glBlendFuncSeparate");
    pfn_glBufferData = (PFN_glBufferData)SDL_GL_GetProcAddress("glBufferData");
    pfn_glBufferSubData = (PFN_glBufferSubData)SDL_GL_GetProcAddress("glBufferSubData");
    pfn_glCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (!pfn_glCheckFramebufferStatus) pfn_glCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatusEXT");
    pfn_glCompileShader = (PFN_glCompileShader)SDL_GL_GetProcAddress("glCompileShader");
    pfn_glCompressedTexImage2D = (PFN_glCompressedTexImage2D)SDL_GL_GetProcAddress("glCompressedTexImage2D");
    pfn_glCreateProgram = (PFN_glCreateProgram)SDL_GL_GetProcAddress("glCreateProgram");
    pfn_glCreateShader = (PFN_glCreateShader)SDL_GL_GetProcAddress("glCreateShader");
    pfn_glDeleteBuffers = (PFN_glDeleteBuffers)SDL_GL_GetProcAddress("glDeleteBuffers");
    pfn_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    if (!pfn_glDeleteFramebuffers) pfn_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)SDL_GL_GetProcAddress("glDeleteFramebuffersEXT");
    pfn_glDisableVertexAttribArray = (PFN_glDisableVertexAttribArray)SDL_GL_GetProcAddress("glDisableVertexAttribArray");
    pfn_glEnableVertexAttribArray = (PFN_glEnableVertexAttribArray)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
    pfn_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    if (!pfn_glFramebufferTexture2D) pfn_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2DEXT");
    pfn_glGenBuffers = (PFN_glGenBuffers)SDL_GL_GetProcAddress("glGenBuffers");
    pfn_glGenFramebuffers = (PFN_glGenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    if (!pfn_glGenFramebuffers) pfn_glGenFramebuffers = (PFN_glGenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffersEXT");
    pfn_glGetActiveUniform = (PFN_glGetActiveUniform)SDL_GL_GetProcAddress("glGetActiveUniform");
    pfn_glGetProgramInfoLog = (PFN_glGetProgramInfoLog)SDL_GL_GetProcAddress("glGetProgramInfoLog");
    pfn_glGetProgramiv = (PFN_glGetProgramiv)SDL_GL_GetProcAddress("glGetProgramiv");
    pfn_glGetShaderInfoLog = (PFN_glGetShaderInfoLog)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    pfn_glGetShaderiv = (PFN_glGetShaderiv)SDL_GL_GetProcAddress("glGetShaderiv");
    pfn_glGetUniformLocation = (PFN_glGetUniformLocation)SDL_GL_GetProcAddress("glGetUniformLocation");
    pfn_glLinkProgram = (PFN_glLinkProgram)SDL_GL_GetProcAddress("glLinkProgram");
    pfn_glShaderSource = (PFN_glShaderSource)SDL_GL_GetProcAddress("glShaderSource");
    pfn_glUniform1f = (PFN_glUniform1f)SDL_GL_GetProcAddress("glUniform1f");
    pfn_glUniform1fv = (PFN_glUniform1fv)SDL_GL_GetProcAddress("glUniform1fv");
    pfn_glUniform1i = (PFN_glUniform1i)SDL_GL_GetProcAddress("glUniform1i");
    pfn_glUniform1iv = (PFN_glUniform1iv)SDL_GL_GetProcAddress("glUniform1iv");
    pfn_glUniform2fv = (PFN_glUniform2fv)SDL_GL_GetProcAddress("glUniform2fv");
    pfn_glUniform2iv = (PFN_glUniform2iv)SDL_GL_GetProcAddress("glUniform2iv");
    pfn_glUniform3fv = (PFN_glUniform3fv)SDL_GL_GetProcAddress("glUniform3fv");
    pfn_glUniform3iv = (PFN_glUniform3iv)SDL_GL_GetProcAddress("glUniform3iv");
    pfn_glUniform4fv = (PFN_glUniform4fv)SDL_GL_GetProcAddress("glUniform4fv");
    pfn_glUniform4iv = (PFN_glUniform4iv)SDL_GL_GetProcAddress("glUniform4iv");
    pfn_glUniformMatrix2fv = (PFN_glUniformMatrix2fv)SDL_GL_GetProcAddress("glUniformMatrix2fv");
    pfn_glUniformMatrix3fv = (PFN_glUniformMatrix3fv)SDL_GL_GetProcAddress("glUniformMatrix3fv");
    pfn_glUniformMatrix4fv = (PFN_glUniformMatrix4fv)SDL_GL_GetProcAddress("glUniformMatrix4fv");
    pfn_glUseProgram = (PFN_glUseProgram)SDL_GL_GetProcAddress("glUseProgram");
    pfn_glVertexAttribPointer = (PFN_glVertexAttribPointer)SDL_GL_GetProcAddress("glVertexAttribPointer");
}

/* __cdecl wrapper implementations */
void CDECL wrap_glActiveTexture(GLenum texture) {
    if (pfn_glActiveTexture) pfn_glActiveTexture(texture);
}

void CDECL wrap_glAttachShader(GLuint program, GLuint shader) {
    if (pfn_glAttachShader) pfn_glAttachShader(program, shader);
}

void CDECL wrap_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    if (pfn_glBindAttribLocation) pfn_glBindAttribLocation(program, index, name);
}

void CDECL wrap_glBindBuffer(GLenum target, GLuint buffer) {
    if (pfn_glBindBuffer) pfn_glBindBuffer(target, buffer);
}

void CDECL wrap_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (pfn_glBindFramebuffer) pfn_glBindFramebuffer(target, framebuffer);
}

void CDECL wrap_glBindTexture(GLenum target, GLuint texture) {
    glBindTexture(target, texture);
}

void CDECL wrap_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    if (pfn_glBlendEquationSeparate) pfn_glBlendEquationSeparate(modeRGB, modeAlpha);
}

void CDECL wrap_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
}

void CDECL wrap_glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    if (pfn_glBlendFuncSeparate) pfn_glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}

void CDECL wrap_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if (pfn_glBufferData) pfn_glBufferData(target, size, data, usage);
}

void CDECL wrap_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    if (pfn_glBufferSubData) pfn_glBufferSubData(target, offset, size, data);
}

GLenum CDECL wrap_glCheckFramebufferStatus(GLenum target) {
    if (pfn_glCheckFramebufferStatus) return pfn_glCheckFramebufferStatus(target);
    return GL_FRAMEBUFFER_COMPLETE;
}

void CDECL wrap_glClear(GLbitfield mask) {
    glClear(mask);
}

void CDECL wrap_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glClearColor(red, green, blue, alpha);
}

void CDECL wrap_glClearDepthf(GLclampf depth) {
    glClearDepth((double)depth);
}

void CDECL wrap_glClearStencil(GLint s) {
    glClearStencil(s);
}

void CDECL wrap_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    glColorMask(red, green, blue, alpha);
}

void CDECL wrap_glCompileShader(GLuint shader) {
    if (pfn_glCompileShader) {
        pfn_glCompileShader(shader);
        GLint status = 0;
        if (pfn_glGetShaderiv) {
            pfn_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (!status && pfn_glGetShaderInfoLog) {
                char log[1024];
                pfn_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
                printf("[GL] Shader compile error: %s\n", log);
            }
        }
    }
}

void CDECL wrap_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    if (pfn_glCompressedTexImage2D) pfn_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

GLuint CDECL wrap_glCreateProgram(void) {
    if (pfn_glCreateProgram) return pfn_glCreateProgram();
    return 0;
}

GLuint CDECL wrap_glCreateShader(GLenum type) {
    if (pfn_glCreateShader) return pfn_glCreateShader(type);
    return 0;
}

void CDECL wrap_glCullFace(GLenum mode) {
    glCullFace(mode);
}

void CDECL wrap_glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    if (pfn_glDeleteBuffers) pfn_glDeleteBuffers(n, buffers);
}

void CDECL wrap_glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    if (pfn_glDeleteFramebuffers) pfn_glDeleteFramebuffers(n, framebuffers);
}

void CDECL wrap_glDeleteTextures(GLsizei n, const GLuint *textures) {
    glDeleteTextures(n, textures);
}

void CDECL wrap_glDepthFunc(GLenum func) {
    glDepthFunc(func);
}

void CDECL wrap_glDepthMask(GLboolean flag) {
    glDepthMask(flag);
}

void CDECL wrap_glDisable(GLenum cap) {
    glDisable(cap);
}

void CDECL wrap_glDisableVertexAttribArray(GLuint index) {
    if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(index);
}

void CDECL wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
}

void CDECL wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    glDrawElements(mode, count, type, indices);
}

void CDECL wrap_glEnable(GLenum cap) {
    glEnable(cap);
}

void CDECL wrap_glEnableVertexAttribArray(GLuint index) {
    if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(index);
}

void CDECL wrap_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    if (pfn_glFramebufferTexture2D) pfn_glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void CDECL wrap_glFrontFace(GLenum mode) {
    glFrontFace(mode);
}

void CDECL wrap_glGenBuffers(GLsizei n, GLuint *buffers) {
    if (pfn_glGenBuffers) pfn_glGenBuffers(n, buffers);
}

void CDECL wrap_glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    if (pfn_glGenFramebuffers) pfn_glGenFramebuffers(n, framebuffers);
}

void CDECL wrap_glGenTextures(GLsizei n, GLuint *textures) {
    glGenTextures(n, textures);
}

void CDECL wrap_glGetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    if (pfn_glGetActiveUniform) pfn_glGetActiveUniform(program, index, bufsize, length, size, type, name);
}

GLenum CDECL wrap_glGetError(void) {
    return glGetError();
}

void CDECL wrap_glGetIntegerv(GLenum pname, GLint *params) {
    glGetIntegerv(pname, params);
}

void CDECL wrap_glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei *length, GLchar *infolog) {
    if (pfn_glGetProgramInfoLog) pfn_glGetProgramInfoLog(program, bufsize, length, infolog);
}

void CDECL wrap_glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    if (pfn_glGetProgramiv) pfn_glGetProgramiv(program, pname, params);
}

void CDECL wrap_glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *infolog) {
    if (pfn_glGetShaderInfoLog) pfn_glGetShaderInfoLog(shader, bufsize, length, infolog);
}

void CDECL wrap_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (pfn_glGetShaderiv) pfn_glGetShaderiv(shader, pname, params);
}

const GLubyte *CDECL wrap_glGetString(GLenum name) {
    return glGetString(name);
}

GLint CDECL wrap_glGetUniformLocation(GLuint program, const GLchar *name) {
    if (pfn_glGetUniformLocation) return pfn_glGetUniformLocation(program, name);
    return -1;
}

void CDECL wrap_glLineWidth(GLfloat width) {
    glLineWidth(width);
}

void CDECL wrap_glLinkProgram(GLuint program) {
    if (pfn_glLinkProgram) {
        pfn_glLinkProgram(program);
        GLint status = 0;
        if (pfn_glGetProgramiv) {
            pfn_glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (!status && pfn_glGetProgramInfoLog) {
                char log[1024];
                pfn_glGetProgramInfoLog(program, sizeof(log), NULL, log);
                printf("[GL] Program link error: %s\n", log);
            }
        }
    }
}

void CDECL wrap_glPixelStorei(GLenum pname, GLint param) {
    glPixelStorei(pname, param);
}

void CDECL wrap_glPolygonOffset(GLfloat factor, GLfloat units) {
    glPolygonOffset(factor, units);
}

void CDECL wrap_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
}

void CDECL wrap_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    if (!string || count <= 0) {
        if (pfn_glShaderSource) pfn_glShaderSource(shader, count, string, length);
        return;
    }
    
    /* Prepend compatibility preamble defining precision qualifiers for desktop OpenGL */
    static const char *preamble = "#version 120\n#define lowp\n#define mediump\n#define highp\n#line 1 0\n";
    const GLchar **new_strings = (const GLchar **)malloc((count + 1) * sizeof(GLchar *));
    GLint *new_lengths = (GLint *)malloc((count + 1) * sizeof(GLint));
    
    new_strings[0] = preamble;
    new_lengths[0] = (GLint)strlen(preamble);
    
    for (int i = 0; i < count; i++) {
        new_strings[i + 1] = string[i];
        new_lengths[i + 1] = (length && length[i] >= 0) ? length[i] : (GLint)strlen(string[i]);
    }
    
    if (pfn_glShaderSource) {
        pfn_glShaderSource(shader, count + 1, new_strings, new_lengths);
    }
    
    free(new_strings);
    free(new_lengths);
}

void CDECL wrap_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void CDECL wrap_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(target, pname, param);
}

void CDECL wrap_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void CDECL wrap_glUniform1f(GLint location, GLfloat v0) {
    if (pfn_glUniform1f) pfn_glUniform1f(location, v0);
}

void CDECL wrap_glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
    if (pfn_glUniform1fv) pfn_glUniform1fv(location, count, value);
}

void CDECL wrap_glUniform1i(GLint location, GLint v0) {
    if (pfn_glUniform1i) pfn_glUniform1i(location, v0);
}

void CDECL wrap_glUniform1iv(GLint location, GLsizei count, const GLint *value) {
    if (pfn_glUniform1iv) pfn_glUniform1iv(location, count, value);
}

void CDECL wrap_glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
    if (pfn_glUniform2fv) pfn_glUniform2fv(location, count, value);
}

void CDECL wrap_glUniform2iv(GLint location, GLsizei count, const GLint *value) {
    if (pfn_glUniform2iv) pfn_glUniform2iv(location, count, value);
}

void CDECL wrap_glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
    if (pfn_glUniform3fv) pfn_glUniform3fv(location, count, value);
}

void CDECL wrap_glUniform3iv(GLint location, GLsizei count, const GLint *value) {
    if (pfn_glUniform3iv) pfn_glUniform3iv(location, count, value);
}

void CDECL wrap_glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    if (pfn_glUniform4fv) pfn_glUniform4fv(location, count, value);
}

void CDECL wrap_glUniform4iv(GLint location, GLsizei count, const GLint *value) {
    if (pfn_glUniform4iv) pfn_glUniform4iv(location, count, value);
}

void CDECL wrap_glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (pfn_glUniformMatrix2fv) pfn_glUniformMatrix2fv(location, count, transpose, value);
}

void CDECL wrap_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (pfn_glUniformMatrix3fv) pfn_glUniformMatrix3fv(location, count, transpose, value);
}

void CDECL wrap_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (pfn_glUniformMatrix4fv) pfn_glUniformMatrix4fv(location, count, transpose, value);
}

void CDECL wrap_glUseProgram(GLuint program) {
    if (pfn_glUseProgram) pfn_glUseProgram(program);
}

void CDECL wrap_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    if (pfn_glVertexAttribPointer) pfn_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void CDECL wrap_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
}
