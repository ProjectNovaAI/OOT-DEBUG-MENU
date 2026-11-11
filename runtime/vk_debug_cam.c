// The scene camera for the debug menu's volume overlay (ui/debug_tab.cpp debug_overlay_draw), whatever the upscaler.
// The game's opaque scene draws bind a 0x3500-byte scene context block at c3 whose rows 0-2 are the view (3x4) and rows
// 7-10 the projection (4x4) their vertex shaders use (vk_dlaa.c ocam_vp reads the same rows for DLSS). A frame can bind
// several such blocks (reflections, other views): the one most draws into the scene depth and a scene colour target use
// this frame is the camera, as in vk_dlaa.c vk_dlaa_draw. vk_dbgcam_draw runs for every scene draw of the replay but
// does nothing until the overlay asks (vk_dbgcam_want), so the cost stays at one load per draw.
#include <stdint.h>
#include <string.h>

#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"
#include "vk_debug_cam.h"

#define SCENE_CONTEXT 0x3500u
#define CAM_FLOATS 44          // rows 0-10
#define VIEWS 8

volatile int vk_dbgcam_want;

static struct {
    uint64_t frame;                       // gfx_counters.frames the counts below belong to
    struct { uint64_t addr; uint32_t n; } v[VIEWS];
    int nv;
    uint32_t best;                        // draws of the block whose rows were taken this frame
    PlatMutex lock;                       // rows, ok, seq
    float rows[CAM_FLOATS];
    int ok;
    uint64_t seq;                         // bumped at every new set of rows
    uint64_t rows_frame;                  // gfx_counters.frames when the rows were taken
} C;

// Frames without a scene draw after which the camera counts as gone (a loading screen, a movie, the title).
#define STALE_FRAMES 3

static int scene_format(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void vk_dbgcam_draw(VkExec *ex) {
    if (!vk_dbgcam_want) return;
    const VkExecTarget *z = &ex->depth;
    const p_bind_buf *b = &ex->ubo[0][0];   // c3
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT) return;
    if (b->size != SCENE_CONTEXT || !b->addr || !ex->color[0].image || !scene_format(ex->color[0].format)) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (fr != C.frame) C.frame = fr, C.nv = 0, C.best = 0;
    int i = 0;
    while (i < C.nv && C.v[i].addr != b->addr) i++;
    if (i == C.nv) {
        if (C.nv == VIEWS) return;
        C.v[C.nv].addr = b->addr, C.v[C.nv++].n = 0;
    }
    uint32_t n = ++C.v[i].n;
    if (n <= C.best) return;
    float r[CAM_FLOATS];
    if (!vk_usnap_read(b->addr, sizeof r, r)) return;
    C.best = n;
    plat_mutex_lock(&C.lock);
    memcpy(C.rows, r, sizeof r);
    C.ok = 1, C.seq++, C.rows_frame = fr;
    plat_mutex_unlock(&C.lock);
}

int vk_dbgcam_vp(float vp[16], float *aspect) {
    vk_dbgcam_want = 1;
    float v[CAM_FLOATS];
    plat_mutex_lock(&C.lock);
    int ok = C.ok;
    uint64_t at = C.rows_frame;
    memcpy(v, C.rows, sizeof v);
    plat_mutex_unlock(&C.lock);
    if (!ok || __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) > at + STALE_FRAMES) return 0;
    // projection rows 7-10 times the view rows 0-2 with (0, 0, 0, 1) under them
    const float *P = v + 7 * 4;
    float V[16] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], 0, 0, 0, 1};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            float acc = 0;
            for (int j = 0; j < 4; j++) acc += P[r * 4 + j] * V[j * 4 + k];
            vp[r * 4 + k] = acc;
        }
    if (aspect) *aspect = P[0] != 0 ? P[5] / P[0] : 16.0f / 9.0f;
    if (aspect && *aspect < 0) *aspect = -*aspect;
    return 1;
}
