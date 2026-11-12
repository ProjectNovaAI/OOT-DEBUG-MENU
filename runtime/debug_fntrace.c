// Function call tracing for the debug console (debug_remote.c "ftrace" and "fcount"), on the lifted code's own
// function-entry hook: every lifted function starts with FN_ENTER (cpu.h), which calls main.c fnprof_hit while
// g_fnprof is set. Two modes, for finding the game code behind a behaviour without stopping any thread:
//   watch: up to WATCH_MAX guest function addresses; every entry of one of them is kept in a ring with its return
//          address (the caller) and x0, newest last.
//   count: every function entered while it runs is counted in a table (the guest address, the entries, the return
//          address and the time of the first one). A behaviour that happens k times in the window shows up as
//          functions entered exactly k times; one that starts once shows up as the functions first entered after it.
// Both cost a call per guest function entry while on, so the game runs slower; neither is kept from one start to
// the next.
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "host.h"
#include "platform/platform.h"

extern volatile int g_fnprof;

#define WATCH_MAX 16
#define RING 4096
#define COUNT_BITS 18          // 262144 slots: more than the functions a game runs in a few seconds
#define COUNT_SLOTS (1u << COUNT_BITS)

static _Atomic int mode;       // 0 off, 1 watch, 2 count
static _Atomic int prof_was;   // g_fnprof before tracing set it
static uint64_t watch[WATCH_MAX];
static _Atomic int nwatch;
static struct { uint64_t addr, lr, x0, ns; } ring[RING];
static _Atomic uint64_t ring_seq;
static struct { _Atomic uint64_t addr; _Atomic uint32_t n; uint64_t lr, ns; } *counts;
static _Atomic uint32_t counted, count_full;
static uint64_t count_t0;   // plat_now_ns at the count's start

void dbg_fntrace_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    int m = atomic_load_explicit(&mode, memory_order_relaxed);
    if (m == 1) {
        int n = atomic_load_explicit(&nwatch, memory_order_relaxed);
        for (int i = 0; i < n; i++)
            if (watch[i] == addr) {
                uint64_t s = atomic_fetch_add(&ring_seq, 1) % RING;
                ring[s].addr = addr, ring[s].lr = lr, ring[s].x0 = x0, ring[s].ns = plat_now_ns();
                return;
            }
    } else if (m == 2 && counts) {
        uint32_t h = (uint32_t)((addr >> 2) * 0x9E3779B1u) >> (32 - COUNT_BITS);
        for (uint32_t probe = 0; probe < 64; probe++, h = (h + 1) & (COUNT_SLOTS - 1)) {
            uint64_t k = atomic_load_explicit(&counts[h].addr, memory_order_relaxed);
            if (k == addr) {
                atomic_fetch_add_explicit(&counts[h].n, 1, memory_order_relaxed);
                return;
            }
            if (!k) {
                uint64_t zero = 0;
                if (atomic_compare_exchange_strong(&counts[h].addr, &zero, addr)) {
                    counts[h].lr = lr;
                    counts[h].ns = plat_now_ns();
                    atomic_fetch_add_explicit(&counts[h].n, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&counted, 1, memory_order_relaxed);
                    return;
                }
                if (zero == addr) {
                    atomic_fetch_add_explicit(&counts[h].n, 1, memory_order_relaxed);
                    return;
                }
            }
        }
        atomic_fetch_add_explicit(&count_full, 1, memory_order_relaxed);
    }
}

static void prof_on(void) {
    if (!atomic_load(&mode)) atomic_store(&prof_was, g_fnprof);
    g_fnprof = 1;
}

void dbg_fntrace_watch(const uint64_t *addrs, int n) {
    if (n > WATCH_MAX) n = WATCH_MAX;
    atomic_store(&mode, 0);
    atomic_store(&nwatch, 0);
    memcpy(watch, addrs, (size_t)n * sizeof *addrs);
    atomic_store(&ring_seq, 0);
    atomic_store(&nwatch, n);
    if (n) prof_on(), atomic_store(&mode, 1);
}

int dbg_fntrace_count_start(void) {
    if (!counts) counts = calloc(COUNT_SLOTS, sizeof *counts);
    if (!counts) return 0;
    atomic_store(&mode, 0);
    memset((void *)counts, 0, COUNT_SLOTS * sizeof *counts);
    atomic_store(&counted, 0), atomic_store(&count_full, 0);
    count_t0 = plat_now_ns();
    prof_on();
    atomic_store(&mode, 2);
    return 1;
}

void dbg_fntrace_off(void) {
    if (atomic_exchange(&mode, 0)) g_fnprof = atomic_load(&prof_was);
}

int dbg_fntrace_mode(void) { return atomic_load(&mode); }

int dbg_fntrace_ring(uint64_t i, uint64_t *addr, uint64_t *lr, uint64_t *x0, uint64_t *ns) {
    uint64_t n = atomic_load(&ring_seq), first = n > RING ? n - RING : 0;
    if (first + i >= n) return 0;
    uint64_t s = (first + i) % RING;
    *addr = ring[s].addr, *lr = ring[s].lr, *x0 = ring[s].x0, *ns = ring[s].ns;
    return 1;
}

int dbg_fntrace_counts(uint32_t lo, uint32_t hi, void (*fn)(uint64_t addr, uint32_t n, uint64_t lr, double first_s, void *ctx), void *ctx,
                       uint32_t *distinct, uint32_t *dropped) {
    int out = 0;
    *distinct = atomic_load(&counted), *dropped = atomic_load(&count_full);
    if (!counts) return 0;
    for (uint32_t h = 0; h < COUNT_SLOTS; h++) {
        uint64_t a = atomic_load(&counts[h].addr);
        uint32_t n = atomic_load(&counts[h].n);
        if (a && n >= lo && n <= hi) fn(a, n, counts[h].lr, counts[h].ns > count_t0 ? (counts[h].ns - count_t0) / 1e9 : 0.0, ctx), out++;
    }
    return out;
}
