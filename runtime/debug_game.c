// The debug tools' frame hook (recomp/patches-v111.txt, set dbgframe): the framework's per-frame update
// (main+0x6e6be0) calls the patch page at +0xf70 in place of its `ldr x8, [x0, #0xa8]`. The host function makes that
// load and then runs debug_warp.c dbg_game_frame on the game thread, where guest functions may be called. Built per install, as
// its address depends on the version's patch page.
#include "debug_tools.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE
#define HOST_DEBUG_FRAME (RT_PATCH_PAGE + 0xf70)

static void debug_frame(Cpu *c) {
    c->x[8] = LD64(c->x[0] + 0xa8);
    dbg_game_frame(c);
}

__attribute__((constructor)) static void debug_frame_register(void) { rt_register_host(HOST_DEBUG_FRAME, debug_frame); }
#endif
