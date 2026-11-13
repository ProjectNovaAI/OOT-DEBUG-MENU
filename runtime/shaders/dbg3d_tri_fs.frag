#version 450
// The debug menu's filled shapes (vk_dbg3d.c).
#define DBG3D_FRAGMENT
#include "dbg3d_common.glsl"

layout(location = 0) in vec4 col;
layout(location = 1) in float vw;

layout(location = 0) out vec4 o;

void main() {
    if (behind_scene(vw)) discard;
    o = vec4(srgb_to_linear(col.rgb), col.a);
}
