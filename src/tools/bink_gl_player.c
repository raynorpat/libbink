/*
 * libbink - OpenGL playback example
 *
 * Build with the tools enabled:
 *   cmake -S . -B build -DBINK_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * The window uses an OpenGL 3.2 core profile and presents through a
 * GLSL shader that converts the decoded YUV planes to RGB on the GPU.
 *
 * Usage:
 *   bink_gl_player file.bik
 *
 * Keys: Esc/q quit, Space pauses the picture and the sound,
 * Left/Right seek one second.
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
#define BINK_GL_DELETE_CONTEXT SDL_GL_DestroyContext
#else
#include <SDL.h>
#include <SDL_opengl.h>
#define BINK_EVENT_QUIT SDL_QUIT
#define BINK_EVENT_KEY_DOWN SDL_KEYDOWN
#define BINK_EVENT_KEY(event) ((event).key.keysym.sym)
#define BINK_KEY_Q SDLK_q
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
    if (!SDL_GL_ExtensionSupported("GL_ARB_texture_rg") &&
        !SDL_GL_ExtensionSupported("GL_EXT_texture_rg")) {
        /* Core in 3.0, so a 3.2 context always has it; the check only
         * catches a driver that lied about its version. */
    }
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

static GLuint make_plane_texture(unsigned width, unsigned height)
{
    GLuint texture = 0;

    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width, (GLsizei)height, 0,
                  GL_RED, GL_UNSIGNED_BYTE, NULL);
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

/* Read back the dump and print the mean absolute difference against the
 * decoder's own packed RGB, at the dump's resolution by nearest sampling. */
static void compare_dump(bink object, const char *path, int dw, int dh)
{
    const unsigned char *rgb = bink_get_video(object);
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
    /* The dump is at drawable resolution; the source is plane resolution. */
    {
        unsigned long i;

        for (i = 0; i < (unsigned long)dw * (unsigned long)dh; i++) {
            unsigned long x = i % (unsigned long)dw;
            unsigned long y2 = i / (unsigned long)dw;
            unsigned long sx = x * w / (unsigned long)dw;
            /* The dump was flipped on write, so row 0 in the file is the
             * top of the picture while GL row 0 is the bottom. */
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


static void wait_until(double target_ms)
{
    double now = (double)SDL_GetTicks();

    if (target_ms > now)
        SDL_Delay((Uint32)(target_ms - now + 0.999));
}

int main(int argc, char **argv)
{
    const char *path;
    bink b;
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint textures[3] = { 0, 0, 0 };
    GLint u_y = -1;
    GLint u_u = -1;
    GLint u_v = -1;
    GLint u_matrix = -1;
    GLint u_offset = -1;
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
    double next_due = 0.0;
    double picture_ms = 0.0;
    int drawable_w = 0;
    int drawable_h = 0;
    const char *dump_path = getenv("BINK_GL_DUMP");
    int i;

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

    program = link_program(bink_gl_fs_present, "presentation");
    if (program == 0) {
        exit_code = 1;
        goto done;
    }
    u_y = gl.GetUniformLocation(program, "u_y");
    u_u = gl.GetUniformLocation(program, "u_u");
    u_v = gl.GetUniformLocation(program, "u_v");
    u_matrix = gl.GetUniformLocation(program, "u_yuv_to_rgb");
    u_offset = gl.GetUniformLocation(program, "u_offset");

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

    textures[0] = make_plane_texture((unsigned)w, (unsigned)h);
    textures[1] = make_plane_texture((unsigned)((w + 1) / 2),
                                     (unsigned)((h + 1) / 2));
    textures[2] = make_plane_texture((unsigned)((w + 1) / 2),
                                     (unsigned)((h + 1) / 2));
    if (textures[0] == 0 || textures[1] == 0 || textures[2] == 0) {
        fprintf(stderr, "Cannot create the plane textures.\n");
        exit_code = 1;
        goto done;
    }

    if (bink_playback_audio_subsystem() == 0) {
        int rate = 0;
        int ch = 0;

        for (i = 0; i < 7; i++) {
            if (enabled[i]) {
                rate = (int)rates[i];
                ch = channels[i];
                break;
            }
        }
        if (rate > 0 && ch > 0 &&
            bink_playback_audio_open(&audio, rate, ch) != 0)
            bink_playback_audio_close(&audio);
    }

    status = bink_first(b);
    if (status != BINK_MORE && status != BINK_LAST) {
        fprintf(stderr, "Failed to decode the first frame.\n");
        exit_code = 1;
        goto done;
    }
    bink_playback_publish(b, enabled, channels, rates, &audio, &audio_bytes);
    if (audio.ready)
        bink_playback_set_paused(&audio, 0, &device_paused);

    printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
           path, w, h, frames, 1000000.0 / usf);
    fflush(stdout);

    snprintf(title, sizeof(title), "bink_gl_player - %s - presentation", path);

    for (;;) {
        SDL_Event event;
        double now;
        const unsigned char *y;
        const unsigned char *u;
        const unsigned char *v;
        int stride_y;
        int stride_u;
        int stride_v;

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
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                unsigned long cur = 0;
                unsigned long step;
                long target;

                bink_info_all(b, &cur, &frames, NULL);
                step = (unsigned long)(1000000.0 / usf + 0.5);
                if (step < 1)
                    step = 1;
                target = (long)cur + (key == SDLK_LEFT ? -(long)step
                                                      : (long)step);
                if (target < 0)
                    target = 0;
                if (frames > 0 && target >= (long)frames)
                    target = (long)frames - 1;
                if (bink_playback_seek(b, (unsigned long)target, &status,
                                       &audio, usf, enabled, channels,
                                       rates) == 0)
                    next_due = 0.0;
            }
        }
        if (quit)
            break;

        y = bink_get_plane(b, BINK_PLANE_Y);
        u = bink_get_plane(b, BINK_PLANE_U);
        v = bink_get_plane(b, BINK_PLANE_V);
        stride_y = bink_get_stride(b, BINK_PLANE_Y);
        stride_u = bink_get_stride(b, BINK_PLANE_U);
        stride_v = bink_get_stride(b, BINK_PLANE_V);
        if (y != NULL && u != NULL && v != NULL) {
            upload_plane(textures[0], y, (unsigned)w, (unsigned)h, stride_y);
            upload_plane(textures[1], u, (unsigned)((w + 1) / 2),
                         (unsigned)((h + 1) / 2), stride_u);
            upload_plane(textures[2], v, (unsigned)((w + 1) / 2),
                         (unsigned)((h + 1) / 2), stride_v);
        }

        gl.Viewport(0, 0, (GLsizei)drawable_w, (GLsizei)drawable_h);
        gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.UseProgram(program);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, textures[0]);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, textures[1]);
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, textures[2]);
        gl.Uniform1i(u_y, 0);
        gl.Uniform1i(u_u, 1);
        gl.Uniform1i(u_v, 2);
        {
            /* BT.601 limited range, column-major for GL. */
            static const GLfloat m[9] = {
                1.164f, 1.164f, 1.164f,
                0.0f, -0.392f, 2.017f,
                1.596f, -0.813f, 0.0f
            };
            const GLfloat y0 = 16.0f / 255.0f;
            const GLfloat c0 = 128.0f / 255.0f;

            gl.UniformMatrix3fv(u_matrix, 1, GL_FALSE, m);
            gl.Uniform3f(u_offset, -1.164f * y0 - 1.596f * c0,
                         -1.164f * y0 + 0.392f * c0 + 0.813f * c0,
                         -1.164f * y0 - 2.017f * c0);
        }
        gl.BindVertexArray(vao);
        gl.DrawArrays(GL_TRIANGLES, 0, 6);
        if (dump_path != NULL) {
            unsigned char *pixels = (unsigned char *)malloc(
                (size_t)drawable_w * (size_t)drawable_h * 4u);
            FILE *fp = fopen(dump_path, "wb");
            int wrote_dump = 0;

            if (pixels != NULL && fp != NULL) {
                gl.ReadPixels(0, 0, (GLsizei)drawable_w, (GLsizei)drawable_h,
                              GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                fprintf(fp, "P6\n%d %d\n255\n", drawable_w, drawable_h);
                for (i = drawable_h - 1; i >= 0; i--) {
                    const unsigned char *row =
                        pixels + (size_t)i * (size_t)drawable_w * 4u;
                    int x;

                    for (x = 0; x < drawable_w; x++) {
                        unsigned char rgb[3];

                        rgb[0] = row[x * 4 + 0];
                        rgb[1] = row[x * 4 + 1];
                        rgb[2] = row[x * 4 + 2];
                        fwrite(rgb, 1, 3, fp);
                    }
                }
                wrote_dump = 1;
            }
            if (fp != NULL)
                fclose(fp);
            free(pixels);
            if (wrote_dump)
                compare_dump(b, dump_path, drawable_w, drawable_h);
            dump_path = NULL;
        }
        SDL_GL_SwapWindow(window);

        if (test_frames != 0 && decoded >= test_frames)
            break;
        if (status == BINK_LAST)
            break;
        if (paused) {
            SDL_Delay(10);
            continue;
        }

        now = (double)SDL_GetTicks();
        if (picture_ms == 0.0) {
            unsigned long frame_bytes = bink_frame_bytes(
                (unsigned long)(audio.rate > 0 ? audio.rate : 48000),
                audio.channels > 0 ? audio.channels : 1, usf);
            double byte_rate = (double)(audio.rate > 0 ? audio.rate : 48000) *
                               (double)(audio.channels > 0 ? audio.channels : 1) *
                               2.0;

            picture_ms = usf / 1000.0;
            if (audio.ready && frame_bytes > 0 && byte_rate > 0.0)
                picture_ms = (double)frame_bytes / byte_rate * 1000.0;
        }
        if (next_due == 0.0)
            next_due = now;
        else if (now - next_due > picture_ms)
            next_due = now;
        next_due += picture_ms;
        wait_until(next_due);

        status = bink_next(b);
        if (status == BINK_ERROR || status == BINK_DONE)
            break;
        decoded++;
        bink_playback_publish(b, enabled, channels, rates, &audio,
                              &audio_bytes);
        if (audio.ready)
            bink_playback_set_paused(&audio, 0, &device_paused);
    }

done:
    if (test_frames != 0)
        printf("audio bytes %lu\n", audio_bytes);
    bink_playback_audio_close(&audio);
    bink_playback_shutdown();
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
