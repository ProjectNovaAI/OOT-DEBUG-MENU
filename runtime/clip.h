#pragma once
// Clips (clip_win.c): the last seconds of the game's picture and sound, kept encoded while the game runs, written to
// an MP4 file when the player asks (the debug menu's hotkey "Save clip", F5 by default). Windows only; elsewhere the
// functions do nothing (clip_sdl.c).
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Present thread (gfx_present.c). clip_frame_due: 1 when a frame should be taken now (recording is on and the last
// one taken is at least a sixtieth of a second old); it starts the recorder on first use. clip_frame hands over a
// frame the GPU has finished writing into the present's clip buffer `buf`: w x h pixels, 4 bytes each, rows `stride`
// bytes apart, of `kind` (CLIP_PX_*), taken at plat_now_ns `ns`. The recorder gives the buffer back with
// gfx_present.c vk_clip_release once it has read it, from its own thread.
enum { CLIP_PX_BGRA, CLIP_PX_RGBA, CLIP_PX_PQ_A2B10, CLIP_PX_PQ_A2R10 };
int clip_frame_due(uint64_t now_ns);
void clip_frame(int buf, const uint8_t *px, int w, int h, int stride, int kind, uint64_t ns);
void vk_clip_release(int buf);

// Audio thread (audio_mixer.c sdl_pull): the frames just sent to the output device, interleaved floats.
void clip_audio(const float *pcm, uint32_t frames, int channels, int rate);

// Saves the last clip_seconds() as an MP4 in the clips folder next to the game, on a thread of its own (the toast
// says when it is written). 0 when nothing is being recorded.
int clip_save(void);

// Recording on or off and the clip length in seconds (settings debug_clip and debug_clip_seconds, kept from one start
// to the next). clip_status: a line for the menu.
void clip_set_enabled(int on);
int clip_enabled(void);
int clip_seconds(void);
void clip_set_seconds(int s);
void clip_status(char *out, int n);
int clip_open_folder(void);   // shows the clips folder (made when missing) in the file manager; 1 on success

#ifdef __cplusplus
}
#endif
