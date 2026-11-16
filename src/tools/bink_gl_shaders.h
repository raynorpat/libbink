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

#endif
