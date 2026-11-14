// Bomb paths (debug_tools.h, "Bomb paths"): while NX2NAVI_DEBUG_VIEW_BOMBS is on, every bomb and Bombchu leaves its
// path through the air, the points where it bounced and where it stopped, for the overlay (ui/debug_tab.cpp
// draw_bombs) and the console (bombs).
//
// A bomb is an actor of the generic actor class named Item_Bomb (a Bombchu: Item_Bombchu), position at +0x31c like any
// actor. The game keeps the object when a bomb goes off and takes it again for the next one pulled out, so a path is
// one flight of an object, not the object: it starts when the bomb leaves the player's hands (HELD_*: not over the
// player any more), and ends when the bomb has not moved for STILL_NS (where it lay and went off) or is back in the
// player's hands. A helper thread finds the objects (a heap pass of about a second, debug_warp.c dbg_find_actors);
// the game thread (dbg_bombs_frame, from the frame hook) reads their positions on every frame, so no bounce is missed.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "platform/platform.h"

#define BOMB_OBJS 8
#define FIND_PAUSE_NS 300000000ull
#define ACTOR_POS 0x31c
#define HELD_RADIUS_M 1.2f      // horizontally this close to the player and HELD_* above the feet: in the hands
#define HELD_LOW_M 0.3f
#define HELD_HIGH_M 2.6f
#define POINT_STEP_M 0.03f      // a new point once the bomb has moved this far
#define BOUNCE_FALL_MS -1.0f    // falling faster than this, then rising or still: a bounce
#define STILL_M 0.01f
#define STILL_NS 300000000ull
#define JUMP_M 8.0f             // moved this far in one frame: the object was taken for a new bomb

static const char *const kNames[] = {"Item_Bomb", "Item_Bombchu"};

static _Atomic uint64_t objs[BOMB_OBJS];
static _Atomic int finder_running;

typedef struct {
    uint64_t obj;
    int flying, path;          // path: the slot in paths being written, while flying
    float last[3], vy;
    uint64_t last_ns, moved_ns;
} Track;
static Track tracks[BOMB_OBJS];

static DbgBombPath paths[DBG_BOMB_PATHS];
static int path_next;            // the slot the next path takes
static uint32_t path_serial;
static PlatMutex lock;

static int view_on(void) { return dbg_available() && cfg_bool("NX2NAVI_DEBUG_VIEW_BOMBS"); }

static void finder(void *arg) {
    (void)arg;
    while (view_on()) {
        uint64_t found[BOMB_OBJS];
        int n = 0;
        for (size_t k = 0; k < sizeof kNames / sizeof *kNames && n < BOMB_OBJS; k++)
            n += dbg_find_actors(kNames[k], found + n, BOMB_OBJS - n);
        for (int i = 0; i < BOMB_OBJS; i++) atomic_store(&objs[i], i < n ? found[i] : 0);
        plat_sleep_ns(FIND_PAUSE_NS);
    }
    for (int i = 0; i < BOMB_OBJS; i++) atomic_store(&objs[i], 0);
    atomic_store(&finder_running, 0);
}

static int in_hands(const float b[3], const float p[3]) {
    float dx = b[0] - p[0], dz = b[2] - p[2], dy = b[1] - p[1];
    return dx * dx + dz * dz < HELD_RADIUS_M * HELD_RADIUS_M && dy > HELD_LOW_M && dy < HELD_HIGH_M;
}

static void add_point(DbgBombPath *pa, const float q[3]) {
    if (pa->n >= DBG_BOMB_POINTS) return;
    memcpy(pa->pt[pa->n++], q, sizeof pa->pt[0]);
}

static void path_end(Track *t) {
    if (!t->flying) return;
    plat_mutex_lock(&lock);
    DbgBombPath *pa = &paths[t->path];
    add_point(pa, t->last);
    memcpy(pa->end, t->last, sizeof pa->end);
    pa->ended = 1;
    plat_mutex_unlock(&lock);
    t->flying = 0;
}

static void path_start(Track *t, const float from[3]) {
    plat_mutex_lock(&lock);
    t->path = path_next;
    path_next = (path_next + 1) % DBG_BOMB_PATHS;
    DbgBombPath *pa = &paths[t->path];
    memset(pa, 0, sizeof *pa);
    pa->serial = ++path_serial;
    pa->used = 1;
    add_point(pa, from);
    plat_mutex_unlock(&lock);
    t->flying = 1;
}

void dbg_bombs_frame(void) {
    if (!view_on()) return;
    int zero = 0;
    if (atomic_compare_exchange_strong(&finder_running, &zero, 1) && !plat_thread_spawn(finder, NULL, "debug-bombs", 0))
        atomic_store(&finder_running, 0);
    float me[3];
    int have_me = dbg_player_pos(me);
    uint64_t now = plat_now_ns();
    for (int i = 0; i < BOMB_OBJS; i++) {
        Track *t = &tracks[i];
        uint64_t o = atomic_load(&objs[i]);
        if (o != t->obj) path_end(t), memset(t, 0, sizeof *t), t->obj = o;
        float q[3];
        if (!o || !plat_mem_read(o + ACTOR_POS, q, sizeof q) || !isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2])) continue;
        if (!t->last_ns) {   // first sight
            memcpy(t->last, q, sizeof q), t->last_ns = t->moved_ns = now;
            continue;
        }
        float dx = q[0] - t->last[0], dy = q[1] - t->last[1], dz = q[2] - t->last[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz), dt = (float)(now - t->last_ns) * 1e-9f;
        int held = have_me && in_hands(q, me);
        if (t->flying && (held || d > JUMP_M)) path_end(t);   // back in the hands: a new bomb in the same object
        if (!t->flying && !held && d > STILL_M && have_me && in_hands(t->last, me)) path_start(t, t->last);
        if (d > STILL_M) t->moved_ns = now;
        if (t->flying) {
            float vy = dt > 0 ? dy / dt : 0;
            plat_mutex_lock(&lock);
            DbgBombPath *pa = &paths[t->path];
            if (t->vy < BOUNCE_FALL_MS && vy >= 0 && pa->bounces < DBG_BOMB_BOUNCES) memcpy(pa->bounce[pa->bounces++], t->last, 12);
            const float *lp = pa->n ? pa->pt[pa->n - 1] : q;
            float sx = q[0] - lp[0], sy = q[1] - lp[1], sz = q[2] - lp[2];
            if (sx * sx + sy * sy + sz * sz >= POINT_STEP_M * POINT_STEP_M) add_point(pa, q);
            plat_mutex_unlock(&lock);
            t->vy = vy;
        }
        memcpy(t->last, q, sizeof q);
        t->last_ns = now;
        if (t->flying && now - t->moved_ns > STILL_NS) path_end(t);
    }
}

int dbg_bomb_paths(DbgBombPath *out, int max) {
    int n = 0;
    plat_mutex_lock(&lock);
    for (int k = 1; k <= DBG_BOMB_PATHS && n < max; k++) {   // newest first
        const DbgBombPath *pa = &paths[(path_next - k + 2 * DBG_BOMB_PATHS) % DBG_BOMB_PATHS];
        if (pa->used) out[n++] = *pa;
    }
    plat_mutex_unlock(&lock);
    return n;
}

void dbg_bomb_paths_clear(void) {
    plat_mutex_lock(&lock);
    for (int k = 0; k < DBG_BOMB_PATHS; k++)
        if (paths[k].ended) paths[k].used = 0;
    plat_mutex_unlock(&lock);
}
