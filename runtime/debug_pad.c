// Scripted controller input for the debug console (debug_tools.h dbg_pad_*; the console's pad). While a script runs,
// it replaces what the controller sends to the game (input.c input_poll), so a sequence can be timed to the frame:
// each step either holds buttons and a left stick position for a number of milliseconds, or holds them until one byte
// of memory reads a value (with a time limit), so a press can follow a game event as soon as it happens. The real
// controller is back the moment the script ends or is stopped. Any game version.
#include <stdatomic.h>
#include <stdint.h>

#include "debug_tools.h"
#include "platform/platform.h"

#define PAD_MAX_STEPS 64

typedef struct {
    uint64_t buttons;      // Switch npad button bits (bit 0 A, 1 B, 9 ZR, 12 D-pad left, ...)
    int32_t lx, ly;        // left stick, -32767..32767, y up
    uint64_t ms;           // how long to hold (the time limit of a wait)
    uint64_t addr;         // wait: the byte to watch, 0 for a timed step
    uint8_t value;         // wait: ends once the byte reads this
} PadStep;

static PadStep steps[PAD_MAX_STEPS];
static int nsteps;
static _Atomic int running, cur;   // cur: the step being held
static uint64_t step_start_ns;     // when the current step began (0: not yet)
static _Atomic uint64_t waited_ms[PAD_MAX_STEPS];   // how long each wait step held before its byte matched

int dbg_pad_add(uint64_t buttons, int32_t lx, int32_t ly, uint64_t ms, uint64_t addr, uint8_t value) {
    if (atomic_load(&running) || nsteps >= PAD_MAX_STEPS) return 0;
    steps[nsteps++] = (PadStep){buttons, lx, ly, ms, addr, value};
    return 1;
}

void dbg_pad_clear(void) {
    atomic_store(&running, 0);
    nsteps = 0;
}

int dbg_pad_run(void) {
    if (!nsteps) return 0;
    for (int i = 0; i < nsteps; i++) atomic_store(&waited_ms[i], 0);
    step_start_ns = 0;
    atomic_store(&cur, 0);
    atomic_store(&running, 1);
    return 1;
}

// The step being held (nsteps once done) and, for wait steps, the milliseconds they held.
int dbg_pad_state(int *step, int *total, uint64_t *waits, int max) {
    *step = atomic_load(&cur);
    *total = nsteps;
    for (int i = 0; i < nsteps && i < max; i++) waits[i] = atomic_load(&waited_ms[i]);
    return atomic_load(&running);
}

// Called from input.c input_poll with what the controller sent; replaces it while a script runs.
void dbg_pad_apply(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!atomic_load(&running)) return;
    uint64_t now = plat_now_ns();
    int i = atomic_load(&cur);
    while (i < nsteps) {
        PadStep *s = &steps[i];
        if (!step_start_ns) step_start_ns = now;
        uint64_t held = (now - step_start_ns) / 1000000;
        int done = held >= s->ms;
        if (s->addr && !done) {
            uint8_t b;
            if (plat_mem_read(s->addr, &b, 1) && b == s->value) {
                atomic_store(&waited_ms[i], held);
                done = 1;
            }
        }
        if (!done) break;
        i++;
        step_start_ns = now;
        atomic_store(&cur, i);
    }
    if (i >= nsteps) {
        atomic_store(&running, 0);
        return;
    }
    *buttons = steps[i].buttons;
    *lx = steps[i].lx, *ly = steps[i].ly;
    *rx = *ry = 0;
}
