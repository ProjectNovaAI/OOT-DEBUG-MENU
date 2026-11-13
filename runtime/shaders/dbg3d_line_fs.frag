#version 450
// The debug menu's lines (vk_dbg3d.c), with a smoothed edge.
#define DBG3D_FRAGMENT
#include "dbg3d_common.glsl"

layout(location = 0) in vec4 col;
layout(location = 1) in float vw;
layout(location = 2) noperspective in float across;
layout(location = 3) flat in float half_w;

layout(location = 0) out vec4 o;

void main() {
    if (behind_scene(vw)) discard;
    float cover = clamp(half_w + 0.5 - abs(across), 0.0, 1.0);
    if (cover <= 0.0) discard;
    o = vec4(srgb_to_linear(col.rgb), col.a * cover);
}
