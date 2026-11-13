#version 450
// The area's collision (vk_dbg3d.c, Show collision): triangles, three corners each in order, each corner 8 bytes in steps
// from the mesh's origin with the triangle's edge bits (dbg3d.h Dbg3dMeshVert). The push constants are in those steps
// for the mesh: the view-projection takes a corner as it is, and the eye and the player are in steps.
#include "dbg3d_common.glsl"

layout(location = 0) in uvec2 q;

layout(location = 0) out vec3 rel;   // from the eye, in steps
layout(location = 1) out float vw;
layout(location = 2) noperspective out vec3 bary;
layout(location = 4) flat out int edges;

invariant gl_Position;   // the depth pass and the colour pass must give the same depth

void main() {
    uint x = q.x & 0x1fffffu;
    uint z = (q.x >> 21) | ((q.y & 0x3ffu) << 11);
    uint y = (q.y >> 10) & 0x7ffffu;
    vec3 pos = vec3(float(x), float(y), float(z));
    vec4 c = to_clip(pos);
    gl_Position = out_pos(c);
    rel = pos - pc.eye.xyz;
    vw = c.w;
    int k = gl_VertexIndex % 3;
    bary = vec3(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);
    edges = int(q.y >> 29);
}
