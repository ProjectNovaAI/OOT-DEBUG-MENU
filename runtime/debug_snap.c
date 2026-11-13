// Frame recorder for the debug console (debug_tools.h dbg_snap_*): on every game frame, from the debug frame hook
// (debug_warp.c dbg_game_frame), it copies up to sixteen chosen memory ranges into a ring, so a change that lasts one
// frame is never missed the way polling from the console's file loop misses it. Any game version.
//
// File written by dbg_snap_save: "NXSNAP1\0", u32 range count, u32 frames kept, then per range u64 address and u64
// length, then per frame (oldest first) u64 time in ns and every range's bytes in order. Unreadable bytes are zero.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "platform/platform.h"

#define SNAP_MAX_RANGES 16
#define SNAP_MAX_FRAME_BYTES 0x20000
#define SNAP_MAX_BYTES (512ull << 20)

static uint64_t rng_addr[SNAP_MAX_RANGES], rng_len[SNAP_MAX_RANGES];
static int nrng;
static size_t frame_bytes;   // the ranges' bytes plus the 8-byte time
static uint32_t frames_cap, frames_done;
static uint8_t *ring;
static _Atomic int on, busy;

int dbg_snap_start(const uint64_t *addr, const uint64_t *len, int n, uint32_t frames) {
    dbg_snap_stop();
    size_t total = 0;
    if (n < 1 || n > SNAP_MAX_RANGES || frames < 1) return 0;
    for (int i = 0; i < n; i++) {
        if (!len[i] || len[i] > SNAP_MAX_FRAME_BYTES) return 0;
        total += len[i];
    }
    if (total > SNAP_MAX_FRAME_BYTES || (uint64_t)(total + 8) * frames > SNAP_MAX_BYTES) return 0;
    free(ring);
    ring = malloc((total + 8) * frames);
    if (!ring) return 0;
    for (int i = 0; i < n; i++) rng_addr[i] = addr[i], rng_len[i] = len[i];
    nrng = n;
    frame_bytes = total + 8;
    frames_cap = frames;
    frames_done = 0;
    atomic_store(&on, 1);
    return 1;
}

// Stops recording and waits for a frame being copied to finish; the ring stays for dbg_snap_save.
void dbg_snap_stop(void) {
    atomic_store(&on, 0);
    while (atomic_load(&busy)) {}
}

uint32_t dbg_snap_frames(void) { return frames_done < frames_cap ? frames_done : frames_cap; }
int dbg_snap_on(void) { return atomic_load(&on); }

void dbg_snap_frame(void) {
    if (!atomic_load(&on)) return;
    atomic_store(&busy, 1);
    if (atomic_load(&on)) {
        uint8_t *p = ring + (size_t)(frames_done % frames_cap) * frame_bytes;
        uint64_t ns = plat_now_ns();
        memcpy(p, &ns, 8);
        p += 8;
        for (int i = 0; i < nrng; i++) {
            if (!plat_mem_read(rng_addr[i], p, rng_len[i])) memset(p, 0, rng_len[i]);
            p += rng_len[i];
        }
        frames_done++;
    }
    atomic_store(&busy, 0);
}

int dbg_snap_save(const char *path) {
    if (!ring) return 0;
    int was_on = atomic_load(&on);
    dbg_snap_stop();
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (was_on) atomic_store(&on, 1);
        return 0;
    }
    uint32_t kept = dbg_snap_frames(), n = (uint32_t)nrng;
    fwrite("NXSNAP1\0", 1, 8, f);
    fwrite(&n, 4, 1, f);
    fwrite(&kept, 4, 1, f);
    for (int i = 0; i < nrng; i++) {
        fwrite(&rng_addr[i], 8, 1, f);
        fwrite(&rng_len[i], 8, 1, f);
    }
    uint32_t first = frames_done - kept;
    for (uint32_t k = 0; k < kept; k++) fwrite(ring + (size_t)((first + k) % frames_cap) * frame_bytes, 1, frame_bytes, f);
    fclose(f);
    if (was_on) atomic_store(&on, 1);
    return 1;
}
