/*
 * libbink - OpenGL playback example
 *
 * Build with the tools enabled:
 *   cmake -S . -B build -DBINK_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * The window uses an OpenGL 3.2 core profile. Two shaders present the
 * decoded planes:
 *   presentation - YUV to RGB on the GPU
 *   interpolation - motion-compensated blend between two decoded frames,
 *                   warped by the decoder's own per-block vectors
 *
 * Usage:
 *   bink_gl_player file.bik
 *
 * Keys: Esc/q quit, Space pauses the picture and the sound,
 *       Left/Right seek one second, M toggles interpolation.
 *
 * Test hooks, all optional environment variables:
 *   BINK_GL_TEST_FRAMES=N  exit after N decoded frames
 *   BINK_GL_SHADER=present|interp  start with that shader
 *   BINK_GL_T=0..1         force the blend factor (implies interp)
 *   BINK_GL_DUMP=path.ppm  write one frame, compare it with the decoder's
 *                          own RGB, and exit
 */

#include "bink.h"
#include "tools/bink_mix.h"
#include "tools/bink_playback.h"
#include "tools/bink_gl_shaders.h"

#ifdef BINK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#define BINK_EVENT_QUIT SDL_EVENT_QUIT
#define BINK_EVENT_KEY_DOWN SDL_EVENT_KEY_DOWN
#define BINK_EVENT_KEY(event) ((event).key.key)
#define BINK_KEY_Q SDLK_Q
#define BINK_KEY_M SDLK_M
#define BINK_GL_DELETE_CONTEXT SDL_GL_DestroyContext
#else
#include <SDL.h>
#include <SDL_opengl.h>
#define BINK_EVENT_QUIT SDL_QUIT
#define BINK_EVENT_KEY_DOWN SDL_KEYDOWN
#define BINK_EVENT_KEY(event) ((event).key.keysym.sym)
#define BINK_KEY_Q SDLK_q
#define BINK_KEY_M SDLK_m
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
    if (fs == 0)
        return 0;
    program = gl.CreateProgram();
    if (program == 0)
        return 0;
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.BindAttribLocation(program, 0, "a_pos");
    gl.LinkProgram(program);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];

        log[0] = '\0';
        gl.GetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "%s program failed to link:\n%s\n", what, log);
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

/* Upload a frame and remember its packed RGB, which the dump mode compares
 * against. */
static void upload_slot(GLuint slots[3], unsigned char *rgb, bink object,
                        unsigned long w, unsigned long h)
{
    const unsigned char *packed;

    upload_frame(slots, object, w, h);
    packed = bink_get_video(object);
    if (packed != NULL)
        memcpy(rgb, packed, (size_t)w * (size_t)h * 3u);
}

/* The frame's motion field as RG8, one texel per 8x8 luma block. */
static void upload_motion(GLuint texture, bink object,
                          unsigned long *blocks_w, unsigned long *blocks_h)
{
    const signed char *mx = NULL;
    const signed char *my = NULL;
    unsigned long bw = 0;
    unsigned long bh = 0;
    unsigned char *texels;
    unsigned long i;

    *blocks_w = 0;
    *blocks_h = 0;
    if (!bink_get_motion(object, &bw, &bh, &mx, &my) || bw == 0 || bh == 0)
        return;
    texels = (unsigned char *)malloc((size_t)bw * (size_t)bh * 2u);
    if (texels == NULL)
        return;
    for (i = 0; i < bw * bh; i++) {
        texels[i * 2u] = (unsigned char)((int)mx[i] + 128);
        texels[i * 2u + 1u] = (unsigned char)((int)my[i] + 128);
    }
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, (int)bw);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (GLsizei)bw, (GLsizei)bh, 0, GL_RG,
                  GL_UNSIGNED_BYTE, texels);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    free(texels);
    *blocks_w = bw;
    *blocks_h = bh;
}

static void wait_until(double target_ms)
{
    double now = (double)SDL_GetTicks();

    if (target_ms > now)
        SDL_Delay((Uint32)(target_ms - now + 0.999));
}

/* Compare a framebuffer dump against the decoder's own packed RGB. */
static void compare_dump(const unsigned char *rgb, const char *path, int dw,
                         int dh)
{
    FILE *fp = fopen(path, "rb");
    unsigned long w = 0;
    unsigned long h = 0;
    int maxv = 0;
    unsigned char *pix = NULL;
    double sum = 0.0;
    double count = 0.0;
    int worst = 0;

    if (rgb == NULL || fp == NULL) {
        if (fp != NULL)
            fclose(fp);
        return;
    }
    if (fscanf(fp, "P6 %lu %lu %d", &w, &h, &maxv) != 3 ||
        w != (unsigned long)dw || h != (unsigned long)dh) {
        fclose(fp);
        return;
    }
    fgetc(fp);
    pix = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
    if (pix == NULL ||
        fread(pix, 1, (size_t)w * (size_t)h * 3u, fp) != (size_t)w * h * 3u) {
        free(pix);
        fclose(fp);
        return;
    }
    fclose(fp);
    {
        unsigned long i;

        for (i = 0; i < (unsigned long)dw * (unsigned long)dh; i++) {
            unsigned long x = i % (unsigned long)dw;
            unsigned long y2 = i / (unsigned long)dw;
            unsigned long sx = x * w / (unsigned long)dw;
            /* The dump is flipped on write, so file row 0 is the top. */
            unsigned long sy =
                ((unsigned long)dh - 1ul - y2) * h / (unsigned long)dh;
            int c;

            for (c = 0; c < 3; c++) {
                int d = (int)pix[(sy * w + sx) * 3u + (unsigned)c] -
                        (int)rgb[(sy * w + sx) * 3u + (unsigned)c];

                if (d < 0)
                    d = -d;
                if (d > worst)
                    worst = d;
                sum += (double)d;
                count += 1.0;
            }
        }
    }
    printf("dump mean abs diff %.2f, worst %d\n", sum / count, worst);
    free(pix);
}

static int dump_framebuffer(const char *path, int dw, int dh)
{
    unsigned char *pixels =
        (unsigned char *)malloc((size_t)dw * (size_t)dh * 4u);
    FILE *fp = fopen(path, "wb");
    int ok = 0;

    if (pixels == NULL || fp == NULL) {
        free(pixels);
        if (fp != NULL)
            fclose(fp);
        return 0;
    }
    gl.ReadPixels(0, 0, (GLsizei)dw, (GLsizei)dh, GL_RGBA, GL_UNSIGNED_BYTE,
                  pixels);
    fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
    {
        int i;

        for (i = dh - 1; i >= 0; i--) {
            const unsigned char *row = pixels + (size_t)i * (size_t)dw * 4u;
            int x;

            for (x = 0; x < dw; x++) {
                unsigned char rgb[3];

                rgb[0] = row[x * 4 + 0];
                rgb[1] = row[x * 4 + 1];
                rgb[2] = row[x * 4 + 2];
                fwrite(rgb, 1, 3, fp);
            }
        }
    }
    fclose(fp);
    free(pixels);
    ok = 1;
    return ok;
}

int main(int argc, char **argv)
{
    const char *path;
    bink b;
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    GLuint program_present = 0;
    GLuint program_interp = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint tex[2][3];
    unsigned char *slot_rgb[2];
    GLuint mv_tex = 0;
    unsigned long blocks_w = 0;
    unsigned long blocks_h = 0;
    unsigned long w = 0;
    unsigned long h = 0;
    unsigned long frames = 0;
    double usf = 0.0;
    unsigned char track_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];
    unsigned long test_frames = 0;
    unsigned long decoded = 1;
    char title[512];
    char status = 0;
    int enabled[7];
    BinkPlayback audio;
    unsigned long audio_bytes = 0;
    int quit = 0;
    int paused = 0;
    int device_paused = 0;
    int exit_code = 0;
    double frame_ms = 0.0;
    double start_ms = 0.0;
    float t = 0.0f;
    int interp = 0;
    int cur = 0;
    int has_next = 0;
    int drawable_w = 0;
    int drawable_h = 0;
    const char *dump_path = getenv("BINK_GL_DUMP");
    const char *force_shader = getenv("BINK_GL_SHADER");
    const char *force_t = getenv("BINK_GL_T");
    const char *test_seek_env = getenv("BINK_GL_TEST_SEEK");
    long test_seek = -1;
    GLint p_y = -1;
    GLint p_u = -1;
    GLint p_v = -1;
    GLint p_matrix = -1;
    GLint p_offset = -1;
    GLint i_y0 = -1;
    GLint i_u0 = -1;
    GLint i_v0 = -1;
    GLint i_y1 = -1;
    GLint i_u1 = -1;
    GLint i_v1 = -1;
    GLint i_mv = -1;
    GLint i_matrix = -1;
    GLint i_offset = -1;
    GLint i_luma = -1;
    GLint i_blocks = -1;
    GLint i_t = -1;
    int i;

    memset(tex, 0, sizeof(tex));
    slot_rgb[0] = NULL;
    slot_rgb[1] = NULL;
    {
        long want = test_seek_env != NULL ? strtol(test_seek_env, NULL, 10) : -1;

        if (want >= 0)
            test_seek = want;
    }
    if (force_shader != NULL && strcmp(force_shader, "present") == 0)
        interp = 0;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s file.bik\n", argv[0]);
        return 1;
    }
    path = argv[1];

    {
        const char *env = getenv("BINK_GL_TEST_FRAMES");

        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long value = strtoul(env, &end, 10);

            if (end != env && value > 0)
                test_frames = value;
        }
    }

    b = bink_open_file(path, BINK_MODE_DISK);
    if (b == NULL) {
        fprintf(stderr, "Errors encountered opening %s, exiting.\n", path);
        return 1;
    }
    bink_info_all(b, NULL, &frames, &usf);
    bink_info_video(b, &w, &h, NULL);
    if (w == 0 || h == 0) {
        fprintf(stderr, "%s has no video track, exiting.\n", path);
        bink_close(b);
        return 1;
    }
    if (usf <= 0.0)
        usf = 1000000.0 / 30.0;
    frame_ms = usf / 1000.0;

    memset(enabled, 0, sizeof(enabled));
    memset(&audio, 0, sizeof(audio));
    bink_info_audio(b, &track_mask, channels, bitdepth, rates);

    if (bink_enable_video(b, 1) != 0) {
        fprintf(stderr, "Failed to enable video decoding.\n");
        bink_close(b);
        return 1;
    }
    for (i = 0; i < 7; i++) {
        if ((track_mask & (1u << i)) == 0 || bitdepth[i] != 16)
            continue;
        enabled[i] = 1;
        if (bink_enable_audio(b, (unsigned char)i, 1) != 0)
            enabled[i] = 0;
    }

#ifdef BINK_USE_SDL3
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        bink_close(b);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    window = SDL_CreateWindow(path, (int)w, (int)h, SDL_WINDOW_OPENGL);
#else
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        bink_close(b);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    window = SDL_CreateWindow(path, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, (int)w, (int)h,
                              SDL_WINDOW_OPENGL);
#endif
    if (window == NULL) {
        fprintf(stderr, "Cannot create the window: %s\n", SDL_GetError());
        exit_code = 1;
        goto done;
    }
    context = SDL_GL_CreateContext(window);
    if (context == NULL) {
        fprintf(stderr, "Cannot create a GL 3.2 context: %s\n", SDL_GetError());
        exit_code = 1;
        goto done;
    }
    SDL_GL_SetSwapInterval(1);
#ifdef BINK_USE_SDL3
    SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
#else
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
#endif
    if (drawable_w <= 0 || drawable_h <= 0) {
        drawable_w = (int)w;
        drawable_h = (int)h;
    }
    if (gl_load() != 0) {
        exit_code = 1;
        goto done;
    }

    program_present = link_program(bink_gl_fs_present, "presentation");
    program_interp = link_program(bink_gl_fs_interpolate, "interpolation");
    if (program_present == 0 || program_interp == 0) {
        exit_code = 1;
        goto done;
    }
    p_y = gl.GetUniformLocation(program_present, "u_y");
    p_u = gl.GetUniformLocation(program_present, "u_u");
    p_v = gl.GetUniformLocation(program_present, "u_v");
    p_matrix = gl.GetUniformLocation(program_present, "u_yuv_to_rgb");
    p_offset = gl.GetUniformLocation(program_present, "u_offset");
    i_y0 = gl.GetUniformLocation(program_interp, "u_y0");
    i_u0 = gl.GetUniformLocation(program_interp, "u_u0");
    i_v0 = gl.GetUniformLocation(program_interp, "u_v0");
    i_y1 = gl.GetUniformLocation(program_interp, "u_y1");
    i_u1 = gl.GetUniformLocation(program_interp, "u_u1");
    i_v1 = gl.GetUniformLocation(program_interp, "u_v1");
    i_mv = gl.GetUniformLocation(program_interp, "u_mv");
    i_matrix = gl.GetUniformLocation(program_interp, "u_yuv_to_rgb");
    i_offset = gl.GetUniformLocation(program_interp, "u_offset");
    i_luma = gl.GetUniformLocation(program_interp, "u_luma_size");
    i_blocks = gl.GetUniformLocation(program_interp, "u_blocks");
    i_t = gl.GetUniformLocation(program_interp, "u_t");

    {
        static const GLfloat quad[] = {
            -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
            -1.0f, 1.0f,  1.0f, -1.0f, 1.0f, 1.0f
        };

        gl.GenVertexArrays(1, &vao);
        gl.BindVertexArray(vao);
        gl.GenBuffers(1, &vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad,
                      GL_STATIC_DRAW);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat),
                               (const void *)0);
    }

    for (i = 0; i < 2; i++) {
        tex[i][0] = make_texture(GL_R8, (unsigned)w, (unsigned)h);
        tex[i][1] = make_texture(GL_R8, (unsigned)((w + 1) / 2),
                                 (unsigned)((h + 1) / 2));
        tex[i][2] = make_texture(GL_R8, (unsigned)((w + 1) / 2),
                                 (unsigned)((h + 1) / 2));
        if (tex[i][0] == 0 || tex[i][1] == 0 || tex[i][2] == 0) {
            fprintf(stderr, "Cannot create the plane textures.\n");
            exit_code = 1;
            goto done;
        }
    }
    slot_rgb[0] = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
    slot_rgb[1] = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
    if (slot_rgb[0] == NULL || slot_rgb[1] == NULL) {
        fprintf(stderr, "Cannot allocate the frame buffers.\n");
        exit_code = 1;
        goto done;
    }
    mv_tex = make_texture(GL_RG8, (unsigned)((w + 7) / 8),
                          (unsigned)((h + 7) / 8));
    if (mv_tex == 0) {
        fprintf(stderr, "Cannot create the motion texture.\n");
        exit_code = 1;
        goto done;
    }

    {
        int rate = 0;
        int ch = 0;

        for (i = 0; i < 7; i++) {
            if (!enabled[i])
                continue;
            rate = (int)rates[i];
            if (channels[i] > 1)
                ch = 2;
        }
        if (ch == 0)
            ch = 1;
        if (rate > 0) {
            if (bink_playback_audio_subsystem() != 0) {
                fprintf(stderr, "%s\n", SDL_GetError());
            } else if (bink_playback_audio_open(&audio, rate, ch) != 0) {
                fprintf(stderr, "%s\n", SDL_GetError());
            } else {
                /* SDL opens the device paused; resume needs to know. */
                device_paused = 1;
            }
        }
    }

    status = bink_first(b);
    if (status != BINK_MORE && status != BINK_LAST) {
        fprintf(stderr, "Failed to decode the first frame.\n");
        exit_code = 1;
        goto done;
    }
    upload_slot(tex[cur], slot_rgb[cur], b, w, h);
    bink_playback_publish(b, enabled, channels, rates, &audio, &audio_bytes);
    if (audio.ready)
        bink_playback_set_paused(&audio, 0, &device_paused);

    start_ms = (double)SDL_GetTicks();
    if (status == BINK_MORE) {
        status = bink_next(b);
        if (status == BINK_MORE || status == BINK_LAST) {
            upload_slot(tex[1], slot_rgb[1], b, w, h);
            upload_motion(mv_tex, b, &blocks_w, &blocks_h);
            has_next = 1;
        }
    }

    printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
           path, w, h, frames, 1000000.0 / usf);
    fflush(stdout);
    snprintf(title, sizeof(title), "bink_gl_player - %s - %s", path,
             interp ? "interpolation" : "presentation");
    SDL_SetWindowTitle(window, title);

    for (;;) {
        SDL_Event event;
        double now;
        int advanced = 0;
        long seek_target = -1;

        while (SDL_PollEvent(&event)) {
            int key;

            if (event.type == BINK_EVENT_QUIT) {
                quit = 1;
                break;
            }
            if (event.type != BINK_EVENT_KEY_DOWN)
                continue;
            key = BINK_EVENT_KEY(event);
            if (key == SDLK_ESCAPE || key == BINK_KEY_Q) {
                quit = 1;
            } else if (key == SDLK_SPACE) {
                paused = !paused;
                bink_playback_set_paused(&audio, paused, &device_paused);
            } else if (key == BINK_KEY_M) {
                interp = !interp;
                snprintf(title, sizeof(title), "bink_gl_player - %s - %s",
                         path, interp ? "interpolation" : "presentation");
                SDL_SetWindowTitle(window, title);
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                unsigned long cur_frame = 0;
                unsigned long step;

                bink_info_all(b, &cur_frame, &frames, NULL);
                step = (unsigned long)(1000000.0 / usf + 0.5);
                if (step < 1)
                    step = 1;
                seek_target = (long)cur_frame +
                              (key == SDLK_LEFT ? -(long)step : (long)step);
            }
        }
        if (quit)
            break;

        if (test_seek >= 0) {
            seek_target = test_seek;
            test_seek = -1;
        }
        if (seek_target >= 0) {
            long target = seek_target;

            seek_target = -1;
            if (target < 0)
                target = 0;
            if (frames > 0 && target >= (long)frames)
                target = (long)frames - 1;
            if (bink_playback_seek(b, (unsigned long)target, &status, &audio,
                                   usf, enabled, channels, rates) == 0) {
                upload_slot(tex[cur], slot_rgb[cur], b, w, h);
                has_next = 0;
                blocks_w = 0;
                blocks_h = 0;
                start_ms = (double)SDL_GetTicks();
                t = 0.0f;
                if (status == BINK_MORE) {
                    char next_status = bink_next(b);

                    if (next_status == BINK_MORE || next_status == BINK_LAST) {
                        status = next_status;
                        upload_slot(tex[cur ^ 1], slot_rgb[cur ^ 1], b, w, h);
                        upload_motion(mv_tex, b, &blocks_w, &blocks_h);
                        has_next = 1;
                        decoded++;
                    }
                }
            }
        }

        now = (double)SDL_GetTicks();
        if (audio.ready && has_next && !paused) {
            /* The queue only vetoes: hold the picture while the device is
             * more than two frames behind, so it can never run away. */
            unsigned long frame_bytes = bink_frame_bytes(
                (unsigned long)audio.rate, audio.channels, usf);

            if (frame_bytes > 0 &&
                bink_playback_queued(&audio) > 2ul * frame_bytes)
                now = start_ms;
        }
        if (force_t != NULL) {
            t = (float)atof(force_t);
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            if (!interp && dump_path != NULL)
                interp = 1;
        } else if (!paused && has_next) {
            double elapsed = now - start_ms;

            if (frame_ms > 0.0)
                t = (float)(elapsed / frame_ms);
            if (t >= 1.0f) {
                t = 0.0f;
                advanced = 1;
            }
        } else if (!has_next) {
            t = 0.0f;
        }

        gl.Viewport(0, 0, (GLsizei)drawable_w, (GLsizei)drawable_h);
        gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.BindVertexArray(vao);
        if (interp && has_next) {
            GLfloat offset[3];
            int next = cur ^ 1;

            gl.UseProgram(program_interp);
            gl.ActiveTexture(GL_TEXTURE0);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][0]);
            gl.ActiveTexture(GL_TEXTURE1);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][1]);
            gl.ActiveTexture(GL_TEXTURE2);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][2]);
            gl.ActiveTexture(GL_TEXTURE3);
            gl.BindTexture(GL_TEXTURE_2D, tex[next][0]);
            gl.ActiveTexture(GL_TEXTURE4);
            gl.BindTexture(GL_TEXTURE_2D, tex[next][1]);
            gl.ActiveTexture(GL_TEXTURE5);
            gl.BindTexture(GL_TEXTURE_2D, tex[next][2]);
            gl.ActiveTexture(GL_TEXTURE6);
            gl.BindTexture(GL_TEXTURE_2D, mv_tex);
            gl.Uniform1i(i_y0, 0);
            gl.Uniform1i(i_u0, 1);
            gl.Uniform1i(i_v0, 2);
            gl.Uniform1i(i_y1, 3);
            gl.Uniform1i(i_u1, 4);
            gl.Uniform1i(i_v1, 5);
            gl.Uniform1i(i_mv, 6);
            yuv_offset(offset);
            gl.UniformMatrix3fv(i_matrix, 1, GL_FALSE, yuv_matrix);
            gl.Uniform3f(i_offset, offset[0], offset[1], offset[2]);
            gl.Uniform2f(i_luma, (GLfloat)w, (GLfloat)h);
            gl.Uniform2i(i_blocks, (GLint)(blocks_w > 0 ? blocks_w : 1),
                         (GLint)(blocks_h > 0 ? blocks_h : 1));
            gl.Uniform1f(i_t, t);
        } else {
            GLfloat offset[3];

            gl.UseProgram(program_present);
            gl.ActiveTexture(GL_TEXTURE0);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][0]);
            gl.ActiveTexture(GL_TEXTURE1);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][1]);
            gl.ActiveTexture(GL_TEXTURE2);
            gl.BindTexture(GL_TEXTURE_2D, tex[cur][2]);
            gl.Uniform1i(p_y, 0);
            gl.Uniform1i(p_u, 1);
            gl.Uniform1i(p_v, 2);
            yuv_offset(offset);
            gl.UniformMatrix3fv(p_matrix, 1, GL_FALSE, yuv_matrix);
            gl.Uniform3f(p_offset, offset[0], offset[1], offset[2]);
        }
        gl.DrawArrays(GL_TRIANGLES, 0, 6);
        if (dump_path != NULL) {
            if (dump_framebuffer(dump_path, drawable_w, drawable_h)) {
                const unsigned char *expect = slot_rgb[cur];

                if (force_t != NULL && atof(force_t) >= 0.5 && has_next)
                    expect = slot_rgb[cur ^ 1];
                compare_dump(expect, dump_path, drawable_w, drawable_h);
            }
            dump_path = NULL;
            if (force_t != NULL)
                break;
        }
        SDL_GL_SwapWindow(window);

        if (advanced) {
            start_ms += frame_ms;
            if (start_ms < now - frame_ms)
                start_ms = now;
            cur ^= 1;
            bink_playback_publish(b, enabled, channels, rates, &audio,
                                  &audio_bytes);
            if (audio.ready)
                bink_playback_set_paused(&audio, 0, &device_paused);
            if (status == BINK_MORE) {
                status = bink_next(b);
                if (status == BINK_MORE || status == BINK_LAST) {
                    upload_slot(tex[cur ^ 1], slot_rgb[cur ^ 1], b, w, h);
                    upload_motion(mv_tex, b, &blocks_w, &blocks_h);
                    decoded++;
                } else {
                    has_next = 0;
                }
            } else {
                has_next = 0;
            }
        }

        if (test_frames != 0 && decoded >= test_frames)
            break;
        /* The last frame holds for one frame time, then playback ends. */
        if (!has_next && status == BINK_LAST &&
            now - start_ms >= frame_ms)
            break;
        if (!advanced)
            wait_until(now + 4.0);
    }

done:
    if (test_frames != 0)
        printf("audio bytes %lu, queued %lu\n", audio_bytes,
               bink_playback_queued(&audio));
    bink_playback_audio_close(&audio);
    bink_playback_shutdown();
    free(slot_rgb[0]);
    free(slot_rgb[1]);
    if (context != NULL)
        BINK_GL_DELETE_CONTEXT(context);
    if (window != NULL)
        SDL_DestroyWindow(window);
    SDL_Quit();
    bink_close(b);
    if (status == BINK_ERROR)
        exit_code = 1;
    return exit_code;
}
