// The scene camera for the debug menu's volume overlay (vk_debug_cam.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct VkExec;
// The replay, for every scene draw (vk_exec_draw.c draw): notes the scene context block the frame's scene draws use.
void vk_dbgcam_draw(struct VkExec *ex);
// The last frame's view-projection, row-major (clip = vp * (x, y, z, 1)), and the projection's width over height.
// 0 while no scene draw was seen, or none in the last few frames (a loading screen). The first call switches the
// tracking on.
int vk_dbgcam_vp(float vp[16], float *aspect);
// Set while the overlay draws (vk_dbgcam_vp sets it); vk_dbgcam_draw does nothing without it.
extern volatile int vk_dbgcam_want;

#ifdef __cplusplus
}
#endif
