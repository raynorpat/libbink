/* OpenGL 3.2 renderer for the shared SDL player. */

#include "tools/bink_gl_renderer.h"

#include "tools/bink_gl_shaders.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#define BINK_GL_DELETE_CONTEXT SDL_GL_DestroyContext
#else
#include <SDL.h>
#include <SDL_opengl.h>
#define BINK_GL_DELETE_CONTEXT SDL_GL_DeleteContext
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL 3.2 entry points are resolved at run time on every platform: Windows
 * exports only GL 1.1 from opengl32, and relying on static prototypes
 * would not link there. */
typedef void (APIENTRYP BINK_PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRYP BINK_PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRYP BINK_PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei,
                                               GLsizei, GLint, GLenum, GLenum,
                                               const void *);
typedef void (APIENTRYP BINK_PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRYP BINK_PFN_glPixelStorei)(GLenum, GLint);
typedef void (APIENTRYP BINK_PFN_glActiveTexture)(GLenum);
typedef GLuint (APIENTRYP BINK_PFN_glCreateShader)(GLenum);
typedef void (APIENTRYP BINK_PFN_glShaderSource)(GLuint, GLsizei,
                                                 const char *const *,
                                                 const GLint *);
typedef void (APIENTRYP BINK_PFN_glCompileShader)(GLuint);
typedef void (APIENTRYP BINK_PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP BINK_PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *,
                                                     char *);
typedef GLuint (APIENTRYP BINK_PFN_glCreateProgram)(void);
typedef void (APIENTRYP BINK_PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRYP BINK_PFN_glBindAttribLocation)(GLuint, GLuint,
                                                       const char *);
typedef void (APIENTRYP BINK_PFN_glLinkProgram)(GLuint);
typedef void (APIENTRYP BINK_PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRYP BINK_PFN_glGetProgramInfoLog)(GLuint, GLsizei,
                                                      GLsizei *, char *);
typedef void (APIENTRYP BINK_PFN_glUseProgram)(GLuint);
typedef GLint (APIENTRYP BINK_PFN_glGetUniformLocation)(GLuint, const char *);
typedef void (APIENTRYP BINK_PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRYP BINK_PFN_glUniform1f)(GLint, GLfloat);
typedef void (APIENTRYP BINK_PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRYP BINK_PFN_glUniform2i)(GLint, GLint, GLint);
typedef void (APIENTRYP BINK_PFN_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRYP BINK_PFN_glUniformMatrix3fv)(GLint, GLsizei, GLboolean,
                                                     const GLfloat *);
typedef void (APIENTRYP BINK_PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRYP BINK_PFN_glBindVertexArray)(GLuint);
typedef void (APIENTRYP BINK_PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRYP BINK_PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRYP BINK_PFN_glBufferData)(GLenum, GLsizeiptr,
                                               const void *, GLenum);
typedef void (APIENTRYP BINK_PFN_glEnableVertexAttribArray)(GLuint);
typedef void (APIENTRYP BINK_PFN_glVertexAttribPointer)(GLuint, GLint, GLenum,
                                                        GLboolean, GLsizei,
                                                        const void *);
typedef void (APIENTRYP BINK_PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRYP BINK_PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRYP BINK_PFN_glClearColor)(GLfloat, GLfloat, GLfloat,
                                               GLfloat);
typedef void (APIENTRYP BINK_PFN_glClear)(GLbitfield);
typedef void (APIENTRYP BINK_PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei,
                                               GLenum, GLenum, void *);
typedef void (APIENTRYP BINK_PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRYP BINK_PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (APIENTRYP BINK_PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void (APIENTRYP BINK_PFN_glDeleteShader)(GLuint);
typedef void (APIENTRYP BINK_PFN_glDeleteProgram)(GLuint);
typedef GLenum (APIENTRYP BINK_PFN_glGetError)(void);

typedef struct {
    BINK_PFN_glGenTextures GenTextures;
    BINK_PFN_glBindTexture BindTexture;
    BINK_PFN_glTexImage2D TexImage2D;
    BINK_PFN_glTexParameteri TexParameteri;
    BINK_PFN_glPixelStorei PixelStorei;
    BINK_PFN_glActiveTexture ActiveTexture;
    BINK_PFN_glCreateShader CreateShader;
    BINK_PFN_glShaderSource ShaderSource;
    BINK_PFN_glCompileShader CompileShader;
    BINK_PFN_glGetShaderiv GetShaderiv;
    BINK_PFN_glGetShaderInfoLog GetShaderInfoLog;
    BINK_PFN_glCreateProgram CreateProgram;
    BINK_PFN_glAttachShader AttachShader;
    BINK_PFN_glBindAttribLocation BindAttribLocation;
    BINK_PFN_glLinkProgram LinkProgram;
    BINK_PFN_glGetProgramiv GetProgramiv;
    BINK_PFN_glGetProgramInfoLog GetProgramInfoLog;
    BINK_PFN_glUseProgram UseProgram;
    BINK_PFN_glGetUniformLocation GetUniformLocation;
    BINK_PFN_glUniform1i Uniform1i;
    BINK_PFN_glUniform1f Uniform1f;
    BINK_PFN_glUniform2f Uniform2f;
    BINK_PFN_glUniform2i Uniform2i;
    BINK_PFN_glUniform3f Uniform3f;
    BINK_PFN_glUniformMatrix3fv UniformMatrix3fv;
    BINK_PFN_glGenVertexArrays GenVertexArrays;
    BINK_PFN_glBindVertexArray BindVertexArray;
    BINK_PFN_glGenBuffers GenBuffers;
    BINK_PFN_glBindBuffer BindBuffer;
    BINK_PFN_glBufferData BufferData;
    BINK_PFN_glEnableVertexAttribArray EnableVertexAttribArray;
    BINK_PFN_glVertexAttribPointer VertexAttribPointer;
    BINK_PFN_glDrawArrays DrawArrays;
    BINK_PFN_glViewport Viewport;
    BINK_PFN_glClearColor ClearColor;
    BINK_PFN_glClear Clear;
    BINK_PFN_glReadPixels ReadPixels;
    BINK_PFN_glDeleteTextures DeleteTextures;
    BINK_PFN_glDeleteBuffers DeleteBuffers;
    BINK_PFN_glDeleteVertexArrays DeleteVertexArrays;
    BINK_PFN_glDeleteShader DeleteShader;
    BINK_PFN_glDeleteProgram DeleteProgram;
    BINK_PFN_glGetError GetError;
} BinkGL;

static BinkGL gl;

/* BT.601 limited range, column-major, and the offset that folds in the
 * 16 and 128 black levels. */
static const GLfloat yuv_matrix[9] = {
    1.164f, 1.164f, 1.164f,
    0.0f, -0.392f, 2.017f,
    1.596f, -0.813f, 0.0f
};

static void yuv_offset(GLfloat out[3])
{
    const GLfloat y0 = 16.0f / 255.0f;
    const GLfloat c0 = 128.0f / 255.0f;

    out[0] = -1.164f * y0 - 1.596f * c0;
    out[1] = -1.164f * y0 + 0.392f * c0 + 0.813f * c0;
    out[2] = -1.164f * y0 - 2.017f * c0;
}

static void *load_symbol(const char *name)
{
    void *p = (void *)SDL_GL_GetProcAddress(name);

    if (p == NULL)
        fprintf(stderr, "GL entry point missing: %s\n", name);
    return p;
}

#define LOAD(field, name)                                                  \
    do {                                                                   \
        gl.field = (BINK_PFN_##name)load_symbol(#name);                    \
        if (gl.field == NULL)                                              \
            return -1;                                                     \
    } while (0)

static int gl_load(void)
{
    LOAD(GenTextures, glGenTextures);
    LOAD(BindTexture, glBindTexture);
    LOAD(TexImage2D, glTexImage2D);
    LOAD(TexParameteri, glTexParameteri);
    LOAD(PixelStorei, glPixelStorei);
    LOAD(ActiveTexture, glActiveTexture);
    LOAD(CreateShader, glCreateShader);
    LOAD(ShaderSource, glShaderSource);
    LOAD(CompileShader, glCompileShader);
    LOAD(GetShaderiv, glGetShaderiv);
    LOAD(GetShaderInfoLog, glGetShaderInfoLog);
    LOAD(CreateProgram, glCreateProgram);
    LOAD(AttachShader, glAttachShader);
    LOAD(BindAttribLocation, glBindAttribLocation);
    LOAD(LinkProgram, glLinkProgram);
    LOAD(GetProgramiv, glGetProgramiv);
    LOAD(GetProgramInfoLog, glGetProgramInfoLog);
    LOAD(UseProgram, glUseProgram);
    LOAD(GetUniformLocation, glGetUniformLocation);
    LOAD(Uniform1i, glUniform1i);
    LOAD(Uniform1f, glUniform1f);
    LOAD(Uniform2f, glUniform2f);
    LOAD(Uniform2i, glUniform2i);
    LOAD(Uniform3f, glUniform3f);
    LOAD(UniformMatrix3fv, glUniformMatrix3fv);
    LOAD(GenVertexArrays, glGenVertexArrays);
    LOAD(BindVertexArray, glBindVertexArray);
    LOAD(GenBuffers, glGenBuffers);
    LOAD(BindBuffer, glBindBuffer);
    LOAD(BufferData, glBufferData);
    LOAD(EnableVertexAttribArray, glEnableVertexAttribArray);
    LOAD(VertexAttribPointer, glVertexAttribPointer);
    LOAD(DrawArrays, glDrawArrays);
    LOAD(Viewport, glViewport);
    LOAD(ClearColor, glClearColor);
    LOAD(Clear, glClear);
    LOAD(ReadPixels, glReadPixels);
    LOAD(DeleteShader, glDeleteShader);
    LOAD(DeleteProgram, glDeleteProgram);
    LOAD(GetError, glGetError);
    LOAD(DeleteTextures, glDeleteTextures);
    LOAD(DeleteBuffers, glDeleteBuffers);
    LOAD(DeleteVertexArrays, glDeleteVertexArrays);
    LOAD(DeleteProgram, glDeleteProgram);
    return 0;
}

static GLuint compile_shader(GLenum type, const char *source, const char *what)
{
    GLuint shader = gl.CreateShader(type);
    GLint ok = 0;

    if (shader == 0) {
        fprintf(stderr, "Cannot create the %s shader\n", what);
        return 0;
    }
    gl.ShaderSource(shader, 1, &source, NULL);
    gl.CompileShader(shader);
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];

        log[0] = '\0';
        gl.GetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "%s shader failed to compile:\n%s\n", what, log);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *fragment, const char *what)
{
    GLuint vs;
    GLuint fs;
    GLuint program;
    GLint ok = 0;

    vs = compile_shader(GL_VERTEX_SHADER, bink_gl_vs, "vertex");
    if (vs == 0)
        return 0;
    fs = compile_shader(GL_FRAGMENT_SHADER, fragment, what);
    if (fs == 0) {
        gl.DeleteShader(vs);
        return 0;
    }
    program = gl.CreateProgram();
    if (program == 0) {
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        return 0;
    }
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.BindAttribLocation(program, 0, "a_pos");
    gl.LinkProgram(program);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];

        log[0] = '\0';
        gl.GetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "%s program failed to link:\n%s\n", what, log);
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

static GLuint make_texture(GLenum internal, unsigned width, unsigned height)
{
    GLuint texture = 0;

    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, (GLsizei)width,
                  (GLsizei)height, 0, internal == GL_RG8 ? GL_RG : GL_RED,
                  GL_UNSIGNED_BYTE, NULL);
    return texture;
}

static void upload_plane(GLuint texture, const unsigned char *data,
                         unsigned width, unsigned height, int stride)
{
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width, (GLsizei)height, 0,
                  GL_RED, GL_UNSIGNED_BYTE, data);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* One YUV texture set: slots[0] = Y, [1] = U, [2] = V. */
static void upload_frame(GLuint slots[3], bink object, unsigned long w,
                         unsigned long h)
{
    const unsigned char *y = bink_get_plane(object, BINK_PLANE_Y);
    const unsigned char *u = bink_get_plane(object, BINK_PLANE_U);
    const unsigned char *v = bink_get_plane(object, BINK_PLANE_V);

    if (y == NULL || u == NULL || v == NULL)
        return;
    upload_plane(slots[0], y, (unsigned)w, (unsigned)h,
                 bink_get_stride(object, BINK_PLANE_Y));
    upload_plane(slots[1], u, (unsigned)((w + 1) / 2),
                 (unsigned)((h + 1) / 2), bink_get_stride(object, BINK_PLANE_U));
    upload_plane(slots[2], v, (unsigned)((w + 1) / 2),
                 (unsigned)((h + 1) / 2), bink_get_stride(object, BINK_PLANE_V));
}

/* The frame's motion field as RG8, one texel per 8x8 luma block. The
 * texel buffer persists; it is sized on the first frame. */
static unsigned char *mv_pixels;
static size_t mv_pixels_cap;

static int upload_motion(GLuint texture, bink object, unsigned long *blocks_w,
                         unsigned long *blocks_h)
{
    const signed char *mx = NULL;
    const signed char *my = NULL;
    unsigned long bw = 0;
    unsigned long bh = 0;
    size_t need;
    unsigned long i;

    *blocks_w = 0;
    *blocks_h = 0;
    if (!bink_get_motion(object, &bw, &bh, &mx, &my) || bw == 0 || bh == 0)
        return -1;
    need = (size_t)bw * (size_t)bh * 2u;
    if (need > mv_pixels_cap) {
        unsigned char *grown = (unsigned char *)realloc(mv_pixels, need);

        if (grown == NULL)
            return -1;
        mv_pixels = grown;
        mv_pixels_cap = need;
    }
    for (i = 0; i < bw * bh; i++) {
        mv_pixels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        mv_pixels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, (int)bw);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (GLsizei)bw, (GLsizei)bh, 0, GL_RG,
                  GL_UNSIGNED_BYTE, mv_pixels);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    *blocks_w = bw;
    *blocks_h = bh;
    return 0;
}


struct BinkRenderer {
    SDL_Window *window;
    SDL_GLContext context;
    GLuint program_present;
    GLuint program_interp;
    GLuint vao;
    GLuint vbo;
    GLuint tex[2][3];
    GLuint mv_tex;
    GLint p_y;
    GLint p_u;
    GLint p_v;
    GLint p_matrix;
    GLint p_offset;
    GLint i_y0;
    GLint i_u0;
    GLint i_v0;
    GLint i_y1;
    GLint i_u1;
    GLint i_v1;
    GLint i_mv;
    GLint i_matrix;
    GLint i_offset;
    GLint i_luma;
    GLint i_blocks;
    GLint i_t;
    unsigned long w;
    unsigned long h;
    int drawable_w;
    int drawable_h;
};

int bink_gl_prepare(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    return 0;
}

BinkRenderer *bink_gl_renderer_create(void *window, unsigned long w,
                                      unsigned long h)
{
    BinkRenderer *r;
    static const GLfloat quad[] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
        -1.0f, 1.0f,  1.0f, -1.0f, 1.0f, 1.0f
    };
    int i;

    r = (BinkRenderer *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->window = (SDL_Window *)window;
    r->w = w;
    r->h = h;
    r->context = SDL_GL_CreateContext(r->window);
    if (r->context == NULL) {
        fprintf(stderr, "Cannot create a GL 3.2 context: %s\n", SDL_GetError());
        bink_gl_renderer_destroy(r);
        return NULL;
    }
    SDL_GL_SetSwapInterval(1);
#ifdef BINK_USE_SDL3
    SDL_GetWindowSizeInPixels(r->window, &r->drawable_w, &r->drawable_h);
#else
    SDL_GL_GetDrawableSize(r->window, &r->drawable_w, &r->drawable_h);
#endif
    if (r->drawable_w <= 0 || r->drawable_h <= 0) {
        r->drawable_w = (int)w;
        r->drawable_h = (int)h;
    }
    if (gl_load() != 0) {
        bink_gl_renderer_destroy(r);
        return NULL;
    }

    r->program_present = link_program(bink_gl_fs_present, "presentation");
    r->program_interp = link_program(bink_gl_fs_interpolate, "interpolation");
    if (r->program_present == 0 || r->program_interp == 0) {
        bink_gl_renderer_destroy(r);
        return NULL;
    }
    r->p_y = gl.GetUniformLocation(r->program_present, "u_y");
    r->p_u = gl.GetUniformLocation(r->program_present, "u_u");
    r->p_v = gl.GetUniformLocation(r->program_present, "u_v");
    r->p_matrix = gl.GetUniformLocation(r->program_present, "u_yuv_to_rgb");
    r->p_offset = gl.GetUniformLocation(r->program_present, "u_offset");
    r->i_y0 = gl.GetUniformLocation(r->program_interp, "u_y0");
    r->i_u0 = gl.GetUniformLocation(r->program_interp, "u_u0");
    r->i_v0 = gl.GetUniformLocation(r->program_interp, "u_v0");
    r->i_y1 = gl.GetUniformLocation(r->program_interp, "u_y1");
    r->i_u1 = gl.GetUniformLocation(r->program_interp, "u_u1");
    r->i_v1 = gl.GetUniformLocation(r->program_interp, "u_v1");
    r->i_mv = gl.GetUniformLocation(r->program_interp, "u_mv");
    r->i_matrix = gl.GetUniformLocation(r->program_interp, "u_yuv_to_rgb");
    r->i_offset = gl.GetUniformLocation(r->program_interp, "u_offset");
    r->i_luma = gl.GetUniformLocation(r->program_interp, "u_luma_size");
    r->i_blocks = gl.GetUniformLocation(r->program_interp, "u_blocks");
    r->i_t = gl.GetUniformLocation(r->program_interp, "u_t");
    if (r->p_y < 0 || r->p_u < 0 || r->p_v < 0 || r->p_matrix < 0 ||
        r->p_offset < 0 || r->i_y0 < 0 || r->i_u0 < 0 || r->i_v0 < 0 ||
        r->i_y1 < 0 || r->i_u1 < 0 || r->i_v1 < 0 || r->i_mv < 0 ||
        r->i_matrix < 0 || r->i_offset < 0 || r->i_luma < 0 ||
        r->i_blocks < 0 || r->i_t < 0) {
        fprintf(stderr, "A shader uniform is missing; cannot present.\n");
        bink_gl_renderer_destroy(r);
        return NULL;
    }
    if (gl.GetError() != GL_NO_ERROR) {
        fprintf(stderr, "GL reported an error during setup.\n");
        bink_gl_renderer_destroy(r);
        return NULL;
    }

    gl.GenVertexArrays(1, &r->vao);
    gl.BindVertexArray(r->vao);
    gl.GenBuffers(1, &r->vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad,
                  GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat),
                           (const void *)0);

    for (i = 0; i < 2; i++) {
        r->tex[i][0] = make_texture(GL_R8, (unsigned)w, (unsigned)h);
        r->tex[i][1] = make_texture(GL_R8, (unsigned)((w + 1) / 2),
                                    (unsigned)((h + 1) / 2));
        r->tex[i][2] = make_texture(GL_R8, (unsigned)((w + 1) / 2),
                                    (unsigned)((h + 1) / 2));
        if (r->tex[i][0] == 0 || r->tex[i][1] == 0 || r->tex[i][2] == 0) {
            fprintf(stderr, "Cannot create the plane textures.\n");
            bink_gl_renderer_destroy(r);
            return NULL;
        }
    }
    r->mv_tex = make_texture(GL_RG8, (unsigned)((w + 7) / 8),
                             (unsigned)((h + 7) / 8));
    if (r->mv_tex == 0) {
        fprintf(stderr, "Cannot create the motion texture.\n");
        bink_gl_renderer_destroy(r);
        return NULL;
    }
    return r;
}

int bink_gl_renderer_upload(BinkRenderer *r, bink object, int slot)
{
    if (r == NULL || slot < 0 || slot > 1)
        return -1;
    upload_frame(r->tex[slot], object, r->w, r->h);
    return 0;
}

int bink_gl_renderer_upload_motion(BinkRenderer *r, bink object,
                                   unsigned long *blocks_w,
                                   unsigned long *blocks_h)
{
    if (r == NULL)
        return -1;
    return upload_motion(r->mv_tex, object, blocks_w, blocks_h);
}

int bink_gl_renderer_draw(BinkRenderer *r, const BinkRenderParams *params)
{
    if (r == NULL || params == NULL)
        return -1;
    gl.Viewport(0, 0, (GLsizei)r->drawable_w, (GLsizei)r->drawable_h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.BindVertexArray(r->vao);
    if (params->interp && params->have_next) {
        GLfloat offset[3];
        int cur = params->slot_cur & 1;
        int next = cur ^ 1;

        gl.UseProgram(r->program_interp);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][0]);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][1]);
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][2]);
        gl.ActiveTexture(GL_TEXTURE3);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[next][0]);
        gl.ActiveTexture(GL_TEXTURE4);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[next][1]);
        gl.ActiveTexture(GL_TEXTURE5);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[next][2]);
        gl.ActiveTexture(GL_TEXTURE6);
        gl.BindTexture(GL_TEXTURE_2D, r->mv_tex);
        gl.Uniform1i(r->i_y0, 0);
        gl.Uniform1i(r->i_u0, 1);
        gl.Uniform1i(r->i_v0, 2);
        gl.Uniform1i(r->i_y1, 3);
        gl.Uniform1i(r->i_u1, 4);
        gl.Uniform1i(r->i_v1, 5);
        gl.Uniform1i(r->i_mv, 6);
        yuv_offset(offset);
        gl.UniformMatrix3fv(r->i_matrix, 1, GL_FALSE, yuv_matrix);
        gl.Uniform3f(r->i_offset, offset[0], offset[1], offset[2]);
        gl.Uniform2f(r->i_luma, (GLfloat)r->w, (GLfloat)r->h);
        gl.Uniform2i(r->i_blocks, (GLint)(params->blocks_w > 0
                                              ? params->blocks_w
                                              : 1),
                     (GLint)(params->blocks_h > 0 ? params->blocks_h : 1));
        gl.Uniform1f(r->i_t, params->t);
    } else {
        GLfloat offset[3];
        int cur = params->slot_cur & 1;

        gl.UseProgram(r->program_present);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][0]);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][1]);
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, r->tex[cur][2]);
        gl.Uniform1i(r->p_y, 0);
        gl.Uniform1i(r->p_u, 1);
        gl.Uniform1i(r->p_v, 2);
        yuv_offset(offset);
        gl.UniformMatrix3fv(r->p_matrix, 1, GL_FALSE, yuv_matrix);
        gl.Uniform3f(r->p_offset, offset[0], offset[1], offset[2]);
    }
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    if (!params->capture)
        SDL_GL_SwapWindow(r->window);
    return 0;
}

int bink_gl_renderer_readback(BinkRenderer *r, unsigned char *rgb,
                              unsigned long w, unsigned long h)
{
    unsigned char *pixels;
    unsigned long y;
    unsigned long x;

    if (r == NULL || rgb == NULL || w == 0 || h == 0)
        return -1;
    pixels = (unsigned char *)malloc((size_t)r->drawable_w *
                                     (size_t)r->drawable_h * 4u);
    if (pixels == NULL)
        return -1;
    gl.ReadPixels(0, 0, (GLsizei)r->drawable_w, (GLsizei)r->drawable_h,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (y = 0; y < h; y++) {
        /* GL row 0 is the bottom; the caller wants top-down. */
        unsigned long src_row =
            ((unsigned long)h - 1ul - y) * (unsigned long)r->drawable_h / h;
        const unsigned char *row =
            pixels + src_row * (size_t)r->drawable_w * 4u;

        for (x = 0; x < w; x++) {
            unsigned long sx = x * (unsigned long)r->drawable_w / w;
            const unsigned char *p = row + sx * 4u;

            rgb[(y * w + x) * 3u + 0] = p[0];
            rgb[(y * w + x) * 3u + 1] = p[1];
            rgb[(y * w + x) * 3u + 2] = p[2];
        }
    }
    free(pixels);
    return 0;
}

void bink_gl_renderer_destroy(BinkRenderer *r)
{
    if (r == NULL)
        return;
    if (r->mv_tex != 0)
        gl.DeleteTextures(1, &r->mv_tex);
    {
        int i;
        int j;

        for (i = 0; i < 2; i++)
            for (j = 0; j < 3; j++)
                if (r->tex[i][j] != 0)
                    gl.DeleteTextures(1, &r->tex[i][j]);
    }
    if (r->vbo != 0)
        gl.DeleteBuffers(1, &r->vbo);
    if (r->vao != 0)
        gl.DeleteVertexArrays(1, &r->vao);
    if (r->program_present != 0)
        gl.DeleteProgram(r->program_present);
    if (r->program_interp != 0)
        gl.DeleteProgram(r->program_interp);
    if (r->context != NULL)
        BINK_GL_DELETE_CONTEXT(r->context);
    free(r);
}
