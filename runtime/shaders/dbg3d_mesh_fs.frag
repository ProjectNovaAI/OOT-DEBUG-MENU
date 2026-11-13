#version 450
// The area's collision (vk_dbg3d.c): each face in the colour of the way it faces (ground green, slopes yellow, walls
// blue, ceilings red), brighter the more squarely it faces the eye and dark grey from behind, so a gap, a missing face or
// one that faces the wrong way shows; the polygons' own edges as lines about a pixel wide. The depth pass (view.w 1)
// keeps every face it draws, so the colour pass shows only the nearest: the edges of what lies behind a face are hidden.
#define DBG3D_FRAGMENT
#include "dbg3d_common.glsl"

layout(location = 0) in vec3 rel;   // from the eye, in the mesh's steps (vk_dbg3d.c)
layout(location = 1) in float vw;
layout(location = 2) noperspective in vec3 bary;
layout(location = 4) flat in int edges;

layout(location = 0) out vec4 o;

void main() {
    // The face's normal from how its position changes across the pixels (taken before any discard, while the whole
    // pixel quad still runs): every point lies in the face, so the two steps span it. Across x then down the picture,
    // their cross product points away from the eye; the other way round, towards it. From behind (the corners run
    // clockwise on the picture) the face's own normal is the opposite one.
    vec3 toward = cross(dFdy(rel), dFdx(rel));
    vec3 fw = max(fwidth(bary), vec3(1e-6));
    vec3 wpos = rel + pc.eye.xyz;
    if (pc.player.w > 0.0 && distance(wpos, pc.player.xyz) > pc.player.w) discard;
    if (behind_scene(vw)) discard;
    if (pc.view.w > 0.5) {   // the depth pass
        o = vec4(0.0);
        return;
    }
    float tl = length(toward);
    vec3 normal = tl > 0.0 ? toward / tl : vec3(0.0, 1.0, 0.0);
    if (!gl_FrontFacing) normal = -normal;
    vec3 to_eye = normalize(-rel);
    float facing = dot(normal, to_eye);
    vec3 col = normal.y > 0.7 ? vec3(70, 220, 110) : normal.y > 0.25 ? vec3(235, 205, 70)
             : normal.y > -0.25 ? vec3(80, 150, 255) : vec3(240, 80, 80);
    col /= 255.0;
    bool back = facing < 0.0;
    vec3 face = back ? vec3(70, 70, 78) / 255.0 : col * (0.45 + 0.55 * facing);
    // distance in pixels to the nearest of the polygon's own edges: edge k runs where corner (k + 2) % 3 weighs 0
    float e = 1e9;
    if ((edges & 1) != 0) e = min(e, bary.z / fw.z);
    if ((edges & 2) != 0) e = min(e, bary.x / fw.x);
    if ((edges & 4) != 0) e = min(e, bary.y / fw.y);
    float line = 1.0 - smoothstep(0.4, 1.4, e);
    int style = int(pc.view.z + 0.5);
    if ((style & 2) != 0) {
        vec3 rgb = (style & 1) != 0 ? mix(face, face * 0.5, line) : face;
        o = vec4(srgb_to_linear(rgb), pc.test.w);
    } else {
        if (line <= 0.0) discard;
        o = vec4(srgb_to_linear(back ? col * 0.5 : col), line * (back ? 0.45 : 0.9));
    }
}
