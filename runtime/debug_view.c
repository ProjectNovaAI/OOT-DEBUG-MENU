// Volume overlay data for the debug menu (Debug tab, View): the placed volumes and objects the game does not draw,
// found among the actors of the generic actor class by name. A helper thread rescans the heap every RESCAN_MS while the
// overlay wants any kind (a scan takes about a second, debug_warp.c dbg_each_actor); the menu reads each listed actor's
// position, rotation and scale every frame (dbg_view_volumes), so moving volumes follow.
//
// Actor layout (game build 1.1.1, measured on Area, AirWall_EnemyPerceptionSight, ForbidAutoPlacementArea*,
// VolumeFogArea, SoundOcclusionArea and Obj_HorseJumpDetectArea): position at +0x31c, the placement rotation as three
// unit columns at +0x328, the placement scale at +0x34c. A volume's scale is its size in metres (an Area of scale
// 80 x 36.6 x 80 covers the open field round the drawbridge); a model's scale stays near 1.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define ACTOR_CLASS 0x4b25e18ull   // the generic actor class (debug_warp.c)
#define ACT_NAME 0x20
#define ACT_POS 0x31c
#define ACT_ROT 0x328
#define ACT_SCALE 0x34c
#define LIST_MAX 1024
#define RESCAN_MS 1500

typedef struct {
    uint64_t actor, name_ptr;
    int kind, shape;
    char name[48], target[48];
} Entry;

static struct {
    PlatMutex lock;                 // list, n
    Entry list[LIST_MAX];
    int n;
    volatile int want;
    volatile int running;
} V;

static int has(const char *s, const char *part) { return strstr(s, part) != NULL; }

// The kind of an actor by its name (0: not shown), and its shape.
static int classify(uint64_t actor, const char *name, int *shape) {
    *shape = DBG_SHAPE_BOX;
    if (has(name, "AirWall") || has(name, "Airwall")) return DBG_VOL_WALL;
    if (!strcmp(name, "Area")) return dbg_area_is_loading_zone(actor) ? DBG_VOL_LOAD : DBG_VOL_TRIGGER;
    // NavMeshCut* (path-finding holes round every character) and ForbidAutoPlacement* (where nothing spawns) fill a
    // town with boxes and say nothing about the level, so they are left out.
    if (has(name, "Area") && !has(name, "AreaBgm") && !has(name, "AreaOverwrite") && !has(name, "VolumeFog") &&
        !has(name, "LocationArea") && !has(name, "NavMeshCut") && !has(name, "ForbidAutoPlacement") &&
        strncmp(name, "EftLoc", 6) != 0) {
        if (has(name, "Cylinder")) *shape = DBG_SHAPE_CYLINDER;
        return DBG_VOL_TRIGGER;
    }
    if (has(name, "HorseJumpForbid")) return DBG_VOL_TRIGGER;
    if (has(name, "HidingHole") || has(name, "Skulltula_Gold") || has(name, "Heart_Piece") || !strncmp(name, "TBox_", 5) ||
        has(name, "Hidden") || has(name, "Secret") || has(name, "MagicBean")) {
        *shape = DBG_SHAPE_MARKER;
        return DBG_VOL_HIDDEN;
    }
    return 0;
}

struct scan {
    Entry *out;
    int n, want;
};

static int scan_one(uint64_t actor, const char *name, void *ctx) {
    struct scan *s = ctx;
    int shape, kind = classify(actor, name, &shape);
    if (!(kind & s->want)) return 1;
    Entry *e = &s->out[s->n++];
    e->actor = actor, e->kind = kind, e->shape = shape;
    plat_mem_read(actor + ACT_NAME, &e->name_ptr, 8);
    strncpy(e->name, name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    e->target[0] = 0;
    uint64_t t = !strcmp(name, "Area") ? dbg_area_target(actor) : 0, cls = 0, tn = 0;
    if (t && plat_mem_read(t, &cls, 8) && cls == MOD_MAIN_BASE + ACTOR_CLASS && plat_mem_read(t + ACT_NAME, &tn, 8) && tn &&
        plat_mem_read(tn, e->target, sizeof e->target))
        e->target[sizeof e->target - 1] = 0;
    else e->target[0] = 0;
    return s->n < LIST_MAX;
}

static void view_thread(void *arg) {
    (void)arg;
    static Entry next[LIST_MAX];
    while (V.want) {
        struct scan s = {next, 0, V.want};
        dbg_each_actor(scan_one, &s);
        plat_mutex_lock(&V.lock);
        memcpy(V.list, next, (size_t)s.n * sizeof *next);
        V.n = s.n;
        plat_mutex_unlock(&V.lock);
        for (int t = 0; t < RESCAN_MS / 50 && V.want; t++) plat_sleep_ms(50);
    }
    __atomic_store_n(&V.running, 0, __ATOMIC_RELEASE);
}

void dbg_view_want(int kinds) {
    V.want = kinds;
    if (!kinds) {
        plat_mutex_lock(&V.lock);
        V.n = 0;
        plat_mutex_unlock(&V.lock);
        return;
    }
    int zero = 0;
    if (__atomic_compare_exchange_n(&V.running, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
        !plat_thread_spawn(view_thread, NULL, "debug-view", 0))
        V.running = 0;
}

static int finite3(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

int dbg_view_volumes(DbgVolume *out, int max, float range) {
    static Entry copy[LIST_MAX];
    float me[3];
    int have_me = dbg_player_pos(me), n = 0, m;
    plat_mutex_lock(&V.lock);
    m = V.n;
    memcpy(copy, V.list, (size_t)m * sizeof *copy);
    plat_mutex_unlock(&V.lock);
    for (int i = 0; i < m && n < max; i++) {
        const Entry *e = &copy[i];
        uint64_t cls = 0, name = 0;
        // still the same actor: the class and the name pointer it had at the scan
        if (!plat_mem_read(e->actor, &cls, 8) || cls != MOD_MAIN_BASE + ACTOR_CLASS) continue;
        if (!plat_mem_read(e->actor + ACT_NAME, &name, 8) || name != e->name_ptr) continue;
        DbgVolume *v = &out[n];
        if (!plat_mem_read(e->actor + ACT_POS, v->pos, sizeof v->pos) || !finite3(v->pos)) continue;
        if (!plat_mem_read(e->actor + ACT_ROT, v->axes, sizeof v->axes) || !finite3(v->axes)) continue;
        if (!plat_mem_read(e->actor + ACT_SCALE, v->scale, sizeof v->scale) || !finite3(v->scale)) continue;
        // pooled actors wait at the origin; a volume far below the world is a fallback
        if (v->pos[0] == 0 && v->pos[1] == 0 && v->pos[2] == 0) continue;
        if (fabsf(v->pos[0]) > 1e5f || fabsf(v->pos[1]) > 1e5f || fabsf(v->pos[2]) > 1e5f) continue;
        if (have_me && range > 0) {
            float d[3] = {v->pos[0] - me[0], v->pos[1] - me[1], v->pos[2] - me[2]};
            float r = range + fmaxf(v->scale[0], fmaxf(v->scale[1], v->scale[2]));   // a big volume reaching into range
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > r * r) continue;
        }
        v->kind = e->kind, v->shape = e->shape;
        memcpy(v->name, e->name, sizeof v->name);
        memcpy(v->target, e->target, sizeof v->target);
        n++;
    }
    return n;
}

// The kinds the settings switch on (nx2navi.ini [debug] debug_view_walls, _loading, _triggers, _hidden), parsed again
// when cfg_version changes; also starts or stops the helper thread's rescans.
int dbg_view_kinds(void) {
    static unsigned seen = ~0u;
    static int kinds;
    unsigned v = cfg_version();
    if (v != seen) {
        seen = v;
        kinds = (cfg_bool("NX2NAVI_DEBUG_VIEW_WALLS") ? DBG_VOL_WALL : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_LOADING") ? DBG_VOL_LOAD : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_TRIGGERS") ? DBG_VOL_TRIGGER : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_HIDDEN") ? DBG_VOL_HIDDEN : 0);
        dbg_view_want(kinds);
    }
    return kinds;
}
