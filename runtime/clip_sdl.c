// Clips (clip.h) off Windows: no encoder is used there yet, so nothing is recorded and the menu says so.
#include "clip.h"

#include <stdio.h>

#include "debug_tools.h"

int clip_frame_due(uint64_t now_ns) { (void)now_ns; return 0; }
void clip_frame(int buf, const uint8_t *px, int w, int h, int stride, int kind, uint64_t ns) {
    (void)px, (void)w, (void)h, (void)stride, (void)kind, (void)ns;
    vk_clip_release(buf);
}
void clip_audio(const float *pcm, uint32_t frames, int channels, int rate) { (void)pcm, (void)frames, (void)channels, (void)rate; }
int clip_save(void) {
    dbg_toast_show("Clips are recorded on Windows only");
    return 0;
}
void clip_set_enabled(int on) { (void)on; }
int clip_enabled(void) { return 0; }
int clip_seconds(void) { return 30; }
void clip_set_seconds(int s) { (void)s; }
void clip_status(char *out, int n) { snprintf(out, (size_t)n, "not available: Windows only"); }
int clip_open_folder(void) { return 0; }
int clip_overlays(void) { return 0; }
void clip_set_overlays(int on) { (void)on; }
int clip_view2(void) { return 0; }
void clip_set_view2(int mode) { (void)mode; }
const char *clip_view2_name(int mode) { (void)mode; return "Off"; }
