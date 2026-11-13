// Shared by the debug menu's 3D shaders (vk_dbg3d.c): the push constants, the projection and the test against the
// scene's view distances. A fragment shader defines DBG3D_FRAGMENT before it includes this.

layout(push_constant) uniform Push {
    vec4 vp[4];    // the view-projection's rows: world to clip (the viewport flips y)
    vec4 eye;      // xyz: the eye; w: render pixels per output pixel (line widths are given in output pixels)
    vec4 player;   // xyz: where the player stands; w: how far from there the collision is drawn (0: everywhere)
    vec4 test;     // x: 1 hides what the scene covers; y, z: the slack, in metres and as a share of the distance; w: opacity
    vec4 view;     // xy: the target's size in pixels; z: style (1 edges, 2 faces); w: 1 in the collision's depth pass
} pc;

const float NEAR_W = 0.05;   // nothing nearer the eye than this (metres along the view) is drawn

vec4 to_clip(vec3 p) {
    vec4 h = vec4(p, 1.0);
    return vec4(dot(pc.vp[0], h), dot(pc.vp[1], h), dot(pc.vp[2], h), dot(pc.vp[3], h));
}

// The position handed to the rasteriser: the depth is 1 - NEAR_W / w, which grows with the distance along the view, so
// the pass's own depth buffer orders what it draws whatever the game's depth convention, and cuts at NEAR_W.
vec4 out_pos(vec4 c) { return vec4(c.x, c.y, c.w - NEAR_W, c.w); }

vec3 srgb_to_linear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

#ifdef DBG3D_FRAGMENT
layout(set = 0, binding = 0, r32f) uniform readonly image2D dist;   // each pixel's view distance, 0 where the sky shows

// Whether the scene at this pixel is nearer than w (with the slack): a wall standing on the ground keeps its foot.
bool behind_scene(float w) {
    if (pc.test.x < 0.5) return false;
    float d = imageLoad(dist, ivec2(gl_FragCoord.xy)).r;
    return d > 0.0 && w > d * (1.0 + pc.test.z) + pc.test.y;
}
#endif
