#version 450
// The debug menu's shapes (vk_dbg3d.c): filled triangles in world space, each corner with its colour (sRGB, alpha).
#include "dbg3d_common.glsl"

layout(location = 0) in vec3 pos;
layout(location = 1) in vec4 rgba;

layout(location = 0) out vec4 col;
layout(location = 1) out float vw;

void main() {
    vec4 c = to_clip(pos);
    gl_Position = out_pos(c);
    col = rgba;
    vw = c.w;
}
