// The debug menu's View overlays drawn into the scene in 3D (vk_dbg3d.c). While an upscaler runs (DLAA, DLSS or FSR
// 3.1: its motion pass gives each pixel's view distance), the area's collision is a mesh on the GPU and the menu's other
// shapes (walls, zones, triggers, hookshot targets...) are handed over each frame as world-space triangles and lines;
// they are drawn into the scene's light target before the upscaler, so they meet the scene per pixel and are smoothed
// with it. Without one the menu draws them flat over the picture itself (ui/debug_tab.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A corner of the collision mesh in 8 bytes: its position in steps of the mesh's scale from its origin
// (vk_dbg3d_mesh_publish), x and z in 21 bits, y in 19, and the triangle's edge bits (bit k: corner k to corner k + 1 is
// the polygon's own edge, not a diagonal of its fan). q[0]: x | z << 21 (z's low 11 bits); q[1]: z >> 11 (10 bits) |
// y << 10 | edges << 29. Kakariko's 4.0M triangles take 96 MB this way (229 MB as floats with a normal).
typedef struct {
    uint32_t q[2];
} Dbg3dMeshVert;

#define DBG3D_MESH_XZ_MAX 0x1fffffu
#define DBG3D_MESH_Y_MAX 0x7ffffu

static inline Dbg3dMeshVert dbg3d_mesh_vert(uint32_t x, uint32_t y, uint32_t z, uint32_t edges) {
    Dbg3dMeshVert v;
    v.q[0] = (x & DBG3D_MESH_XZ_MAX) | (z & 0x7ffu) << 21;
    v.q[1] = (z & DBG3D_MESH_XZ_MAX) >> 11 | (y & DBG3D_MESH_Y_MAX) << 10 | (edges & 7u) << 29;
    return v;
}

typedef struct {
    float p[3];
    uint32_t rgba;   // sRGB colour and alpha, R in the low byte (ImGui's IM_COL32)
} Dbg3dVert;

typedef struct {
    float a[3], b[3];
    uint32_t rgba;
    float width;   // output pixels
} Dbg3dLine;

// 1 while the 3D path has run within the last few frames: the menu hands its shapes over instead of drawing them.
int vk_dbg3d_ready(void);

// The collision mesh, from the debug hook's thread: room for verts corners (three a triangle) that the caller fills,
// then publish hands it to the next frame with the world position of step 0 and the size of a step in metres (or drop,
// which also clears the mesh shown). NULL when there is no room.
Dbg3dMeshVert *vk_dbg3d_mesh_alloc(uint32_t verts);
void vk_dbg3d_mesh_publish(const float origin[3], float scale);
void vk_dbg3d_mesh_drop(void);
uint32_t vk_dbg3d_mesh_triangles(void);   // the mesh on the GPU, 0 none
// The size in MB of a mesh left off the GPU, or taken off it, because the game's video memory had no room for it; 0
// when none was. The menu then draws the nearest polygons itself (ui/debug_tab.cpp draw_collision).
uint32_t vk_dbg3d_mesh_no_room_mb(void);

// This frame's shapes from the menu (any thread): triangles (three corners each) and lines. occlude: hidden where the
// scene is nearer (Only what you can see). They are drawn until the next call, or for a moment if none comes.
void vk_dbg3d_submit(const Dbg3dVert *tri, int ntri_verts, const Dbg3dLine *lines, int nlines, int occlude);

#ifdef VK_VERSION_1_0
// vk_dlaa.c evaluate, after the motion pass: the shapes into the light target (view cv, format fmt, w x h, in GENERAL)
// against the view distances dv (R32F, GENERAL; dist_ok 0: none valid), the scene drawn with the jitter jx, jy (render
// pixels); ow: the output width (line widths are in output pixels).
void vk_dbg3d_draw(VkCommandBuffer cb, VkImageView cv, VkFormat fmt, uint32_t w, uint32_t h, uint32_t ow, VkImageView dv,
                   int dist_ok, float jx, float jy);
// gfx_present.c record_clip: the clips' second view (mode 1 above the player, 2 beside, 3 in front; see vk_dbg3d.c)
// drawn on the present's command buffer for fence slot `slot` and blitted into the lower right corner of dst, the clip
// frame of dw x dh in dst_fmt and TRANSFER_DST layout, which it leaves in that layout. 0 when nothing was drawn (no
// player, an HDR swapchain, or the view could not be set up).
int vk_dbg3d_view2(VkCommandBuffer cb, unsigned slot, int mode, VkImage dst, VkFormat dst_fmt, int dw, int dh);
void vk_dbg3d_release(void);   // the device is idle
#endif

#ifdef __cplusplus
}
#endif
