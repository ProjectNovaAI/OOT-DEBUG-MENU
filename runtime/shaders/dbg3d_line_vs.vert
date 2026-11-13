#version 450
// The debug menu's lines (vk_dbg3d.c): one instance a line, six corners (two triangles) a pixel wider than the line on
// each side for its smoothed edge, across the line as it is on the picture. A line reaching behind the eye is cut where
// it comes nearer than NEAR_W.
#include "dbg3d_common.glsl"

layout(location = 0) in vec3 a;
layout(location = 1) in vec3 b;
layout(location = 2) in vec4 rgba;
layout(location = 3) in float width;   // output pixels

layout(location = 0) out vec4 col;
layout(location = 1) out float vw;
layout(location = 2) noperspective out float across;   // pixels from the middle of the line
layout(location = 3) flat out float half_w;             // half its width, render pixels

const vec2 corner[6] = vec2[](vec2(0, -1), vec2(1, -1), vec2(1, 1), vec2(0, -1), vec2(1, 1), vec2(0, 1));

void main() {
    vec4 ca = to_clip(a), cb = to_clip(b);
    col = rgba;
    if (ca.w < NEAR_W && cb.w < NEAR_W) {   // all behind the eye: outside the view
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vw = 0.0, across = 0.0, half_w = 0.0;
        return;
    }
    if (ca.w < NEAR_W) ca = mix(ca, cb, (NEAR_W - ca.w) / (cb.w - ca.w));
    if (cb.w < NEAR_W) cb = mix(cb, ca, (NEAR_W - cb.w) / (ca.w - cb.w));
    vec2 size = pc.view.xy;
    vec2 sa = ca.xy / ca.w * 0.5 * size, sb = cb.xy / cb.w * 0.5 * size;
    vec2 d = sb - sa;
    float len = length(d);
    vec2 dir = len > 1e-4 ? d / len : vec2(1.0, 0.0);
    vec2 perp = vec2(-dir.y, dir.x);
    vec2 k = corner[gl_VertexIndex % 6];
    vec4 c = k.x < 0.5 ? ca : cb;
    half_w = 0.5 * width * pc.eye.w;
    float out_px = half_w + 1.0;
    c.xy += perp * (k.y * out_px) * 2.0 / size * c.w;
    gl_Position = out_pos(c);
    vw = c.w;
    across = k.y * out_px;
}
