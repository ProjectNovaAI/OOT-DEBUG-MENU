// Debug tools of the in-game menu (debug_tools.h). Game build 1.1.1 only: the player actor is found the way
// frame_rate.c finds it ([[main+0x4e371d0] + 0x480], name "Player" at +0x20, position floats at +0x31c).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define PLAYER_MANAGER_SLOT 0x4e371d0   // main+: pointer to the object that holds the player actor
#define PLAYER_ACTOR_FIELD 0x480        // in that object
#define ACTOR_NAME_FIELD 0x20
#define ACTOR_POS_FIELD 0x31c
#define SPEED_WINDOW_NS 100000000ull    // the speed is the displacement over about this long
#define TELEPORT_JUMP_M 20.0f

int dbg_available(void) { return RT_V111; }

static int guest_read(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }

static uint64_t guest_ptr(uint64_t a) {
    uint64_t p = 0;
    return guest_read(a, &p, 8) ? p : 0;
}

static uint64_t player_actor(void) {
    if (!dbg_available()) return 0;
    uint64_t manager = guest_ptr(MOD_MAIN_BASE + PLAYER_MANAGER_SLOT);
    uint64_t actor = manager ? guest_ptr(manager + PLAYER_ACTOR_FIELD) : 0;
    char name[7];
    uint64_t name_ptr = actor ? guest_ptr(actor + ACTOR_NAME_FIELD) : 0;
    if (!name_ptr || !guest_read(name_ptr, name, sizeof name) || memcmp(name, "Player", sizeof name)) return 0;
    return actor;
}

int dbg_player_pos(float pos[3]) {
    uint64_t actor = player_actor();
    if (!actor || !guest_read(actor + ACTOR_POS_FIELD, pos, 12)) return 0;
    return isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]);
}

int dbg_player_set_pos(const float pos[3]) {
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return 0;
    uint64_t actor = player_actor();
    if (!actor) return 0;
    uint64_t at = actor + ACTOR_POS_FIELD, end;
    if (plat_mem_state(at & ~0xFFFull, (at & ~0xFFFull) + 0x2000, &end) != PLAT_PROT_RW) return 0;   // the heap: read-write
    plat_mem_prepare_write(at, 12);
    memcpy((void *)(uintptr_t)at, pos, 12);
    return 1;
}

// ---- speed

static float last_pos[3];
static uint64_t last_ns, window_ns;
static float window_from[3];
static int have_last;
static _Atomic float ground_speed, vertical_speed;

void dbg_tick(uint64_t now_ns) {
    float pos[3];
    if (!dbg_player_pos(pos)) {
        have_last = 0;
        atomic_store(&ground_speed, 0.0f);
        atomic_store(&vertical_speed, 0.0f);
        return;
    }
    float dx = pos[0] - last_pos[0], dy = pos[1] - last_pos[1], dz = pos[2] - last_pos[2];
    if (!have_last || sqrtf(dx * dx + dy * dy + dz * dz) > TELEPORT_JUMP_M) {   // first sight or a jump: start over
        window_ns = now_ns;
        memcpy(window_from, pos, sizeof pos);
        atomic_store(&ground_speed, 0.0f);
        atomic_store(&vertical_speed, 0.0f);
    } else if (now_ns - window_ns >= SPEED_WINDOW_NS) {
        double dt = (double)(now_ns - window_ns) * 1e-9;
        float wx = pos[0] - window_from[0], wy = pos[1] - window_from[1], wz = pos[2] - window_from[2];
        atomic_store(&ground_speed, (float)(sqrt((double)wx * wx + (double)wz * wz) / dt));
        atomic_store(&vertical_speed, (float)(wy / dt));
        window_ns = now_ns;
        memcpy(window_from, pos, sizeof pos);
    }
    memcpy(last_pos, pos, sizeof pos);
    last_ns = now_ns;
    have_last = 1;
}

float dbg_ground_speed(void) { return atomic_load(&ground_speed); }
float dbg_vertical_speed(void) { return atomic_load(&vertical_speed); }

// ---- saved positions

static float slot_pos[DBG_SLOTS][3];
static int slot_set[DBG_SLOTS];

int dbg_slot_save(int slot) {
    if (slot < 0 || slot >= DBG_SLOTS) return 0;
    float pos[3];
    if (!dbg_player_pos(pos)) return 0;
    memcpy(slot_pos[slot], pos, sizeof pos);
    slot_set[slot] = 1;
    return 1;
}

int dbg_slot_load(int slot) {
    if (slot < 0 || slot >= DBG_SLOTS || !slot_set[slot]) return 0;
    return dbg_player_set_pos(slot_pos[slot]);
}

int dbg_slot_used(int slot, float pos[3]) {
    if (slot < 0 || slot >= DBG_SLOTS || !slot_set[slot]) return 0;
    if (pos) memcpy(pos, slot_pos[slot], 12);
    return 1;
}

// ---- game speed

static _Atomic float time_scale = 1.0f;

float dbg_time_scale(void) { return atomic_load_explicit(&time_scale, memory_order_relaxed); }

void dbg_set_time_scale(float scale) {
    if (!isfinite(scale)) scale = 1.0f;
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 8.0f) scale = 8.0f;
    atomic_store_explicit(&time_scale, scale, memory_order_relaxed);
}

// ---- timer

static int timer_on;
static uint64_t timer_start_ns, timer_acc_ns;
static uint64_t timer_start_ticks, timer_acc_ticks;
static uint64_t lap_ns[DBG_LAPS];
static int lap_n;

int dbg_timer_running(void) { return timer_on; }

uint64_t dbg_timer_ns(void) { return timer_acc_ns + (timer_on ? plat_now_ns() - timer_start_ns : 0); }

uint64_t dbg_timer_ticks(void) { return timer_acc_ticks + (timer_on ? frame_step_ticks() - timer_start_ticks : 0); }

void dbg_timer_toggle(void) {
    if (timer_on) {
        timer_acc_ns += plat_now_ns() - timer_start_ns;
        timer_acc_ticks += frame_step_ticks() - timer_start_ticks;
        timer_on = 0;
    } else {
        timer_start_ns = plat_now_ns();
        timer_start_ticks = frame_step_ticks();
        timer_on = 1;
    }
}

void dbg_timer_reset(void) {
    timer_on = 0;
    timer_acc_ns = timer_acc_ticks = 0;
    lap_n = 0;
}

void dbg_timer_lap(void) {
    if (lap_n < DBG_LAPS) lap_ns[lap_n++] = dbg_timer_ns();
}

int dbg_timer_laps(uint64_t ns[DBG_LAPS]) {
    memcpy(ns, lap_ns, sizeof lap_ns);
    return lap_n;
}

// ---- overlay

static _Atomic int overlay_mask;

int dbg_overlay_mask(void) { return atomic_load_explicit(&overlay_mask, memory_order_relaxed); }

void dbg_overlay_set(int mask) { atomic_store_explicit(&overlay_mask, mask, memory_order_relaxed); }
