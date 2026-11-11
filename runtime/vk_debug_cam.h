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
// The scene's view distances (vk_dlaa.c odepth_probe) for an overlay that hides what the scene covers: a grid of
// *gw by *gh (row 0 the top of the view, metres along the view direction, 0 where the view shows sky) into out, which
// holds 480 * 270 floats. 1 when a grid read back within max_age_ns is there; 0 while none is (no upscaler with its
// motion pass runs: DLAA, DLSS or FSR). Each call asks for the next second of grids.
int vk_dbgcam_depth(float *out, int *gw, int *gh, unsigned long long max_age_ns);

#ifdef __cplusplus
}
#endif
