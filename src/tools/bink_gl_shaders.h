#ifndef BINK_GL_SHADERS_H
#define BINK_GL_SHADERS_H

/* GLSL 150: the floor for an OpenGL 3.2 core profile, which is the
 * highest core profile macOS exposes at that level. */

static const char *bink_gl_vs =
    "#version 150\n"
    "in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    /* Plane row 0 is the top of the picture; GL's y axis points up. */\n"
    "    v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/* YUV to RGB. The matrix and offset arrive as uniforms so the BT.601
 * constants live in one C place. */
static const char *bink_gl_fs_present =
    "#version 150\n"
    "uniform sampler2D u_y;\n"
    "uniform sampler2D u_u;\n"
    "uniform sampler2D u_v;\n"
    "uniform mat3 u_yuv_to_rgb;\n"
    "uniform vec3 u_offset;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec3 yuv = vec3(texture(u_y, v_uv).r,\n"
    "                    texture(u_u, v_uv).r,\n"
    "                    texture(u_v, v_uv).r);\n"
    "    vec3 rgb = u_yuv_to_rgb * yuv + u_offset;\n"
    "    o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

/* Motion-compensated interpolation between two decoded frames. The motion
 * texture is the newer frame's field, one RG8 texel per 8x8 luma block,
 * stored as mv + 128. Each output pixel warps both frames toward the
 * display time and blends them. */
static const char *bink_gl_fs_interpolate =
    "#version 150\n"
    "uniform sampler2D u_y0;\n"
    "uniform sampler2D u_u0;\n"
    "uniform sampler2D u_v0;\n"
    "uniform sampler2D u_y1;\n"
    "uniform sampler2D u_u1;\n"
    "uniform sampler2D u_v1;\n"
    "uniform sampler2D u_mv;\n"
    "uniform mat3 u_yuv_to_rgb;\n"
    "uniform vec3 u_offset;\n"
    "uniform vec2 u_luma_size;\n"
    "uniform vec2 u_chroma_size;\n"
    "uniform ivec2 u_blocks;\n"
    "uniform float u_t;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    ivec2 b = ivec2(floor(v_uv * u_luma_size / 8.0));\n"
    "    b = clamp(b, ivec2(0), u_blocks - 1);\n"
    "    vec2 pixels = texelFetch(u_mv, b, 0).rg * 255.0 - 128.0;\n"
    "    vec2 mv_luma = u_t * pixels / u_luma_size;\n"
    "    vec2 mv_chroma = u_t * pixels / u_chroma_size;\n"
    "    vec2 uv0l = v_uv + mv_luma;\n"
    "    vec2 uv1l = v_uv - (1.0 - u_t) * mv_luma;\n"
    "    vec2 uv0c = v_uv + mv_chroma;\n"
    "    vec2 uv1c = v_uv - (1.0 - u_t) * mv_chroma;\n"
    "    vec3 yuv0 = vec3(texture(u_y0, uv0l).r, texture(u_u0, uv0c).r,\n"
    "                     texture(u_v0, uv0c).r);\n"
    "    vec3 yuv1 = vec3(texture(u_y1, uv1l).r, texture(u_u1, uv1c).r,\n"
    "                     texture(u_v1, uv1c).r);\n"
    "    vec3 yuv = mix(yuv0, yuv1, u_t);\n"
    "    vec3 rgb = u_yuv_to_rgb * yuv + u_offset;\n"
    "    o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

#endif
