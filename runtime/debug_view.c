// Volume overlay data for the debug menu (Debug tab, View): the placed volumes and objects the game does not draw,
// found among the actors of the generic actor class by name (and, for some death barriers, in the stage files). A helper thread rescans the heap every RESCAN_MS while the
// overlay wants any kind (a scan takes about a second, debug_warp.c dbg_each_actor); the menu reads each listed actor's
// position, rotation and scale every frame (dbg_view_volumes), so moving volumes follow.
//
// Actor layout (game build 1.1.1, measured on Area, AirWall_EnemyPerceptionSight, ForbidAutoPlacementArea*,
// VolumeFogArea, SoundOcclusionArea and Obj_HorseJumpDetectArea): position at +0x31c, the placement rotation as three
// unit columns at +0x328, the placement scale at +0x34c. A volume's scale is its size in metres (an Area of scale
// 80 x 36.6 x 80 covers the open field round the drawbridge); a model's scale stays near 1.
//
// Death barriers (from the actor packs' shape parameters in the game data): Exterminator is the kill volume under every
// area, a box of half extents 5975 x 500 x 4975 m over its position (y 0 to 1000) whose surface material disables
// respawning; MinusObj_EldinHardLavaPlane_* is a 10 x 10 m plate from y -1 to 0 at the surface of the lava (the crater's
// is scaled 10, the lake). Obj_Abyss_* is only the black bottom drawn in a pit (no physics of its own) and the
// quicksand's shape is a mesh, so those two are marked, not outlined. Exterminator is a live actor; the lava planes and
// pit bottoms never are (passive placements), so those come from the area's stage files (debug_place.c), read once per
// area change.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "clip.h"
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
#define PLACED_MAX 256
#define RESCAN_MS 1500

typedef struct {
    uint64_t actor, name_ptr;   // actor 0: a placement from the stage files, at pos / axes / scale below
    int kind, shape;
    float pos[3], axes[9], scale[3];
    float base[3];    // the base shape's full size the placement scale multiplies (1 for a volume)
    float lift;       // where the shape starts on the up axis, in units of the scaled height
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

// A death barrier by its name: 1 with the shape, the name to show and the base shape's size set in e.
static int death_of(const char *name, int *shape, Entry *e) {
    *shape = DBG_SHAPE_BOX;
    e->base[0] = e->base[1] = e->base[2] = 1.0f, e->lift = 0;
    const char *label = NULL;
    if (!strcmp(name, "Exterminator")) {
        e->base[0] = 11950.0f, e->base[1] = 1000.0f, e->base[2] = 9950.0f;
        label = "Death plane (kill volume under the world)";
    } else if (has(name, "HardLavaPlane")) {
        e->base[0] = 10.0f, e->base[2] = 10.0f, e->lift = -1.0f;
        label = "Lava";
    } else if (!strncmp(name, "Obj_Abyss", 9)) {
        *shape = DBG_SHAPE_MARKER;
        label = "Bottomless pit";
    } else if (has(name, "QuickSand")) {
        *shape = DBG_SHAPE_MARKER;
        label = "Quicksand";
    }
    if (!label) return 0;
    strncpy(e->name, label, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    return 1;
}

// Placed air wall actors the player never meets, by a part of their names. Each pack says what its body collides with
// (Phive/RigidBodyEntityParam EnableLayerHitMask and EnableSubLayerHitMask, followed through their parents): these
// collide only with enemies (HitOnlyEnemy, HitOnlyEnemyWithDedicatedAirWall, HitOnlyDodongo, HitOnlyPoe,
// HitOnlyDungeonBossGohma), NPCs, animals, cuccos, the horse, characters' sight, small objects, the hookshot, bombs or
// rolling rocks; the camera wall has the camera's own material, the lure wall is for the fishing line and
// AirWall_EnemyPerceptionSight for what enemies see. FldObj_AirWall_Player*, _ClimbPlayer, _All, _Common_Cube,
// _ExcludeNpc*, _CrawlHole, the PushAirWall_* and the areas' boundary walls stop the player and stay.
static const char *const kWallNotPlayer[] = {
    "AirWallEnemy", "AirWall_EnemyPerceptionSight", "AirWall_DedicatedEnemy", "AirWall_EnemyDodongo", "AirWall_EnemyPoe",
    "AirWall_OnlyDungeonBossGohma", "AirWall_Npc", "AirWall_BaseForNPC", "AirWall_Animal", "AirWall_Cucco",
    "AirWall_Horse", "AirWall_HidingHoleCover", "AirWall_CharacterSight", "AirWall_CameraSight",
    "AirWall_ObjectAndSmallObject", "MoveFloor_A_01_Airwall", "AirWall_Hookshot", "AirWall_HitOnlyBomb",
    "AirWall_RollingRock", "AirWall_LureAndFishingLine", "FishingPond_AirWall"};

static int wall_stops_player(const char *name) {
    for (size_t i = 0; i < sizeof kWallNotPlayer / sizeof *kWallNotPlayer; i++)
        if (has(name, kWallNotPlayer[i])) return 0;
    return 1;
}

// The kind of a live actor by its name (0: not shown), its shape and, for the death barriers, the name to show and
// the base shape's size.
static int classify(uint64_t actor, const char *name, int *shape, Entry *e) {
    if (!strcmp(name, "Exterminator")) return death_of(name, shape, e) ? DBG_VOL_DEATH : 0;
    *shape = DBG_SHAPE_BOX;
    e->base[0] = e->base[1] = e->base[2] = 1.0f, e->lift = 0;
    if (!strncmp(name, "Enemy_", 6) || !strncmp(name, "DungeonBoss", 11) || has(name, "MiddleBoss")) return DBG_VOL_ENEMY;
    // Breakable rocks, walls, floors and rubble (the WallCrack family) have a life component too: shown with the
    // enemies' health. The grotto covers built on the same base stay among the hidden things below.
    if (has(name, "BrokenRock") || has(name, "CrackWall") || has(name, "CrackFloor") || has(name, "BrokenRubble") ||
        !strcmp(name, "WallCrack"))
        return DBG_VOL_ENEMY;
    if (has(name, "AirWall") || has(name, "Airwall")) return wall_stops_player(name) ? DBG_VOL_WALL : 0;
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
    Entry *e = &s->out[s->n];
    e->name[0] = 0;
    int shape, kind = classify(actor, name, &shape, e);
    if (!(kind & s->want)) return 1;
    s->n++;
    e->actor = actor, e->kind = kind, e->shape = shape;
    plat_mem_read(actor + ACT_NAME, &e->name_ptr, 8);
    if (!e->name[0]) {
        strncpy(e->name, name, sizeof e->name - 1);
        e->name[sizeof e->name - 1] = 0;
    }
    e->target[0] = 0;
    uint64_t t = !strcmp(name, "Area") ? dbg_area_target(actor) : 0, cls = 0, tn = 0;
    if (t && plat_mem_read(t, &cls, 8) && cls == MOD_MAIN_BASE + ACTOR_CLASS && plat_mem_read(t + ACT_NAME, &tn, 8) && tn &&
        plat_mem_read(tn, e->target, sizeof e->target))
        e->target[sizeof e->target - 1] = 0;
    else e->target[0] = 0;
    return s->n < LIST_MAX;
}

// The death barriers among an area's placements (not Exterminator, which is a live actor).
static Entry placed[PLACED_MAX];
static int placed_n;

static int place_one(const DbgPlacement *pl, void *ctx) {
    (void)ctx;
    Entry *e = &placed[placed_n];
    int shape;
    if (pl->void_out) {   // a box like the live Area actor's (its placement scale, the base shape 1 m)
        shape = DBG_SHAPE_BOX;
        e->base[0] = e->base[1] = e->base[2] = 1.0f, e->lift = 0;
        snprintf(e->name, sizeof e->name, "Void-out zone");
    } else if (!strcmp(pl->name, "Exterminator") || !death_of(pl->name, &shape, e))
        return 1;
    e->actor = 0, e->name_ptr = 0, e->kind = DBG_VOL_DEATH, e->shape = shape, e->target[0] = 0;
    memcpy(e->pos, pl->pos, sizeof e->pos);
    memcpy(e->axes, pl->axes, sizeof e->axes);
    memcpy(e->scale, pl->scale, sizeof e->scale);
    return ++placed_n < PLACED_MAX;
}

static void view_thread(void *arg) {
    (void)arg;
    static Entry next[LIST_MAX];
    static char placed_area[64];
    while (V.want) {
        struct scan s = {next, 0, V.want};
        char area[64];
        if ((V.want & DBG_VOL_DEATH) && dbg_current_area(area) && area[0] && strcmp(area, placed_area)) {
            placed_n = 0;
            int got = dbg_place_each(area, place_one, NULL);
            memcpy(placed_area, area, sizeof placed_area);
            rt_log("debug view: %s: %d death barriers among %d placements", area, placed_n, got);
        }
        if (V.want & DBG_VOL_DEATH)
            for (int i = 0; i < placed_n && s.n < LIST_MAX; i++) next[s.n++] = placed[i];
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
        DbgVolume *v = &out[n];
        if (e->kind == DBG_VOL_ENEMY) continue;   // drawn by dbg_view_health
        if (!e->actor) {   // a placement: where the stage file puts it
            memcpy(v->pos, e->pos, sizeof v->pos);
            memcpy(v->axes, e->axes, sizeof v->axes);
            memcpy(v->scale, e->scale, sizeof v->scale);
        } else {
            // still the same actor: the class and the name pointer it had at the scan
            if (!plat_mem_read(e->actor, &cls, 8) || cls != MOD_MAIN_BASE + ACTOR_CLASS) continue;
            if (!plat_mem_read(e->actor + ACT_NAME, &name, 8) || name != e->name_ptr) continue;
            if (!plat_mem_read(e->actor + ACT_POS, v->pos, sizeof v->pos) || !finite3(v->pos)) continue;
            if (!plat_mem_read(e->actor + ACT_ROT, v->axes, sizeof v->axes) || !finite3(v->axes)) continue;
            if (!plat_mem_read(e->actor + ACT_SCALE, v->scale, sizeof v->scale) || !finite3(v->scale)) continue;
        }
        // pooled actors wait at the origin; a volume far below the world is a fallback
        if (v->pos[0] == 0 && v->pos[1] == 0 && v->pos[2] == 0) continue;
        if (fabsf(v->pos[0]) > 1e5f || fabsf(v->pos[1]) > 1e5f || fabsf(v->pos[2]) > 1e5f) continue;
        for (int j = 0; j < 3; j++) v->scale[j] *= e->base[j];
        for (int j = 0; j < 3; j++) v->pos[j] += v->axes[3 + j] * v->scale[1] * e->lift;
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

// Enemy health (debug_tools.h DbgHealth): the life component by the path the game's life query takes, checked by its
// class, then the current and maximum values.
#define LIFE_HOLDER 0x270
#define LIFE_COMPONENT 0x2b8
#define LIFE_CLASS 0x4b4c460ull
#define LIFE_CUR 0x1c10
#define LIFE_MAX 0x1c18

// at (NULL: not wanted): the address of the current value, for dbg_one_hit_tick.
static int life_of(uint64_t actor, int *life, int *max, uint64_t *at) {
    uint64_t holder = 0, comp = 0, cls = 0, pc = 0, pm = 0;
    uint32_t c = 0, m = 0;
    if (!plat_mem_read(actor + LIFE_HOLDER, &holder, 8) || !holder || !plat_mem_read(holder + LIFE_COMPONENT, &comp, 8) || !comp ||
        !plat_mem_read(comp, &cls, 8) || cls != MOD_MAIN_BASE + LIFE_CLASS || !plat_mem_read(comp + LIFE_CUR, &pc, 8) || !pc ||
        !plat_mem_read(comp + LIFE_MAX, &pm, 8) || !pm || !plat_mem_read(pc + 8, &c, 4) || !plat_mem_read(pm + 8, &m, 4))
        return 0;
    if (m == 0 || m > 100000 || c > 100000) return 0;
    *life = (int)c, *max = (int)m;
    if (at) *at = pc + 8;
    return 1;
}

int dbg_view_health(DbgHealth *out, int max, float range) {
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
        DbgHealth *h = &out[n];
        if (e->kind != DBG_VOL_ENEMY || !e->actor) continue;
        if (!plat_mem_read(e->actor, &cls, 8) || cls != MOD_MAIN_BASE + ACTOR_CLASS) continue;
        if (!plat_mem_read(e->actor + ACT_NAME, &name, 8) || name != e->name_ptr) continue;
        if (!plat_mem_read(e->actor + ACT_POS, h->pos, sizeof h->pos) || !finite3(h->pos)) continue;
        if (h->pos[0] == 0 && h->pos[1] == 0 && h->pos[2] == 0) continue;   // pooled, waiting at the origin
        if (have_me && range > 0) {
            float d[3] = {h->pos[0] - me[0], h->pos[1] - me[1], h->pos[2] - me[2]};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > range * range) continue;
        }
        if (!life_of(e->actor, &h->life, &h->max, NULL)) continue;
        memcpy(h->name, e->name, sizeof h->name);
        n++;
    }
    return n;
}

// The kinds the settings switch on (nx2navi.ini [debug] debug_view_walls, _loading, _triggers, _hidden, _death,
// _health, _hookshot, _collision), parsed again when cfg_version changes; also starts or stops the helper thread's rescans.
int dbg_view_kinds(void) {
    static unsigned seen = ~0u;
    static int kinds;
    unsigned v = cfg_version();
    if (v != seen) {
        seen = v;
        kinds = (cfg_bool("NX2NAVI_DEBUG_VIEW_WALLS") ? DBG_VOL_WALL : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_LOADING") ? DBG_VOL_LOAD : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_TRIGGERS") ? DBG_VOL_TRIGGER : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_HIDDEN") ? DBG_VOL_HIDDEN : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_DEATH") ? DBG_VOL_DEATH : 0) |
                (cfg_bool("NX2NAVI_DEBUG_VIEW_HEALTH") ? DBG_VOL_ENEMY : 0);
        dbg_view_want(kinds | (cfg_bool("NX2NAVI_DEBUG_ONE_HIT") ? DBG_VOL_ENEMY : 0));   // the overlay's set stays as it is
        // debug_hook.c, its own thread: the hookshot surfaces and the walls of the collision files
        // and the collision for the clips' second view (vk_dbg3d.c vk_dbg3d_view2) while clips are recorded with it
        int view2 = clip_enabled() && clip_view2();
        dbg_hook_want((cfg_bool("NX2NAVI_DEBUG_VIEW_HOOKSHOT") ? DBG_HOOK_SURFACES : 0) |
                      (cfg_bool("NX2NAVI_DEBUG_VIEW_WALLS") ? DBG_HOOK_WALLS : 0) |
                      (cfg_bool("NX2NAVI_DEBUG_VIEW_COLLISION") || view2 ? DBG_HOOK_COLLISION : 0));
        if (cfg_bool("NX2NAVI_DEBUG_VIEW_HOOKSHOT")) kinds |= DBG_VOL_HOOK;
        if (cfg_bool("NX2NAVI_DEBUG_VIEW_COLLISION")) kinds |= DBG_VOL_COLLISION;
    }
    return kinds;
}

// One-hit kills (debug_tools.h dbg_one_hit). The list comes from the helper thread's rescans (every RESCAN_MS), and
// the life of each listed enemy is read and lowered on every presented frame, so one that heals or is reset is caught
// again before the player's next swing lands. Enemies already at 1 or at 0 (dying) are left alone.
void dbg_one_hit(int on) { dbg_keep("NX2NAVI_DEBUG_ONE_HIT", on ? "1" : "0"); }
int dbg_one_hit_on(void) { return cfg_bool("NX2NAVI_DEBUG_ONE_HIT") != 0; }

void dbg_one_hit_tick(void) {
    dbg_view_kinds();   // follows a change of the setting: starts or stops the enemy scan
    if (!dbg_one_hit_on()) return;
    static uint64_t actors[LIST_MAX], names[LIST_MAX];
    int n = 0;
    plat_mutex_lock(&V.lock);
    for (int i = 0; i < V.n; i++)
        if (V.list[i].kind == DBG_VOL_ENEMY && V.list[i].actor)
            actors[n] = V.list[i].actor, names[n++] = V.list[i].name_ptr;
    plat_mutex_unlock(&V.lock);
    for (int i = 0; i < n; i++) {
        uint64_t cls = 0, name = 0, at = 0;
        int life, max;
        // still the same actor (a freed one's memory may hold another by now)
        if (!plat_mem_read(actors[i], &cls, 8) || cls != MOD_MAIN_BASE + ACTOR_CLASS) continue;
        if (!plat_mem_read(actors[i] + ACT_NAME, &name, 8) || name != names[i]) continue;
        if (life_of(actors[i], &life, &max, &at) && life > 1) dbg_write32(at, 1);
    }
}
