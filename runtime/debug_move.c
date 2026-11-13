// Fly, hover and infinite Hover Boots for the debug menu (debug_tools.h), with the No void-out and Bomb T-Pose settings.
// Game build 1.1.1 only.
//
// Fly and hover put the player where they want with the game's own actor warp (debug_warp.c dbg_frame_teleport) on
// every frame, from the debug frame hook (debug_game.c, game thread), so the physics body goes along and gravity never
// gets to act. Each keeps its own position and moves it with the left stick: fly at its own speed and also up and down,
// hover at a fixed height and about running speed (the game gives a player in the air no walking speed). Infinite Hover
// Boots leaves the player to the game and only makes the boots' own hover time endless (boots_frame).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"

#define BTN_A (1ull << 0)
#define BTN_B (1ull << 1)
#define BTN_X (1ull << 2)
#define STICK_DEAD 0.15f
#define FRAME_DT_MAX 0.1f           // seconds: a longer gap (a loading screen) moves nothing
#define CAMERA_NAME "PlayerCamera"  // its position is the eye: the player minus it is the view direction

static _Atomic int fly_on, hover_on, boots_on, no_void_on, bomb_bug_on;
static _Atomic float fly_speed = 8.0f;

void dbg_fly(int on) { atomic_store(&fly_on, on != 0); }
int dbg_fly_on(void) { return atomic_load(&fly_on); }

// The screen the game's UI is on: an enum flag without a known name (key 0x578d41e1) whose values are None, ItemMenu,
// EquipmentMenu, CollectMenu, SystemMenu, Save, Load, Option, ..., Ocarina, Title, Loading, GameOver, stored as the
// hash of the name (dbg_flag_key). It reads Ocarina from the moment the ocarina is taken out until it is put away, and
// None again then (watched in the game, 1.1.1).
#define UI_SCREEN_KEY 0x578d41e1u
#define UI_SCREEN_OCARINA 0x6a586f83u   // dbg_flag_key("Ocarina")

uint64_t dbg_fly_game_mask(void) {
    uint32_t screen;
    if (!atomic_load(&fly_on) || !dbg_flag_get_u32(DBG_FLAG_ENUM, UI_SCREEN_KEY, &screen)) return 0;
    return screen == UI_SCREEN_OCARINA ? BTN_B : 0;
}
void dbg_hover(int on) { atomic_store(&hover_on, on != 0); }
int dbg_hover_on(void) { return atomic_load(&hover_on); }
// Infinite Hover Boots, No void-out, Bomb T-Pose and the fly speed stay as set from one start to the next (debug_hover_boots,
// debug_fly_speed, debug_no_void_out, debug_bomb_bug; debug_tools.c dbg_keep). Fly and hover do not: a start in the air
// or inside a wall would surprise.
static void move_load(void) {
    static _Atomic int done;
    if (atomic_load(&done) || atomic_exchange(&done, 1)) return;
    atomic_store(&boots_on, cfg_bool("NX2NAVI_DEBUG_HOVER_BOOTS") != 0);
    atomic_store(&no_void_on, cfg_bool("NX2NAVI_DEBUG_NO_VOID_OUT") != 0);
    atomic_store(&bomb_bug_on, cfg_bool("NX2NAVI_DEBUG_BOMB_BUG") != 0);
    float ms = strtof(cfg_value("NX2NAVI_DEBUG_FLY_SPEED"), NULL);
    atomic_store(&fly_speed, ms >= 1.0f && ms <= 100.0f ? ms : 8.0f);
}

void dbg_hover_boots(int on) {
    move_load();
    atomic_store(&boots_on, on != 0);
    dbg_keep("NX2NAVI_DEBUG_HOVER_BOOTS", on ? "1" : "0");
}
int dbg_hover_boots_on(void) { move_load(); return atomic_load(&boots_on); }
// No void-out: read by debug_game.c debug_abyss_hits, the host function of the abyss areas' patch site, on every
// frame such an area is near; kept from one start to the next (debug_no_void_out).
void dbg_no_void_out(int on) {
    move_load();
    atomic_store(&no_void_on, on != 0);
    dbg_keep("NX2NAVI_DEBUG_NO_VOID_OUT", on ? "1" : "0");
}
int dbg_no_void_out_on(void) { move_load(); return atomic_load(&no_void_on); }
// Bomb T-Pose: bomb_frame below, on every frame; kept from one start to the next (debug_bomb_bug).
void dbg_bomb_bug(int on) {
    move_load();
    atomic_store(&bomb_bug_on, on != 0);
    dbg_keep("NX2NAVI_DEBUG_BOMB_BUG", on ? "1" : "0");
}
int dbg_bomb_bug_on(void) { move_load(); return atomic_load(&bomb_bug_on); }
float dbg_fly_speed(void) { move_load(); return atomic_load(&fly_speed); }
void dbg_set_fly_speed(float ms) {
    move_load();
    ms = ms < 1.0f ? 1.0f : ms > 100.0f ? 100.0f : ms;
    atomic_store(&fly_speed, ms);
    char v[16];
    snprintf(v, sizeof v, "%g", ms);
    dbg_keep("NX2NAVI_DEBUG_FLY_SPEED", v);
}

// ---- the camera actor, found by a helper thread and checked on every use

static _Atomic uint64_t camera;
static _Atomic int camera_looking;

static void camera_thread(void *arg) {
    (void)arg;
    uint64_t a[2] = {0, 0};
    if (dbg_find_actors(CAMERA_NAME, a, 2) > 0) atomic_store(&camera, a[0]);
    atomic_store(&camera_looking, 0);
}

static int camera_pos(float out[3]) {
    uint64_t c = atomic_load(&camera);
    if (c && dbg_actor_named(c, CAMERA_NAME) && plat_mem_read(c + 0x31c, out, 12) && isfinite(out[0]) && isfinite(out[2])) return 1;
    atomic_store(&camera, 0);
    int zero = 0;
    if (atomic_compare_exchange_strong(&camera_looking, &zero, 1) && !plat_thread_spawn(camera_thread, NULL, "debug-camera", 0))
        atomic_store(&camera_looking, 0);
    return 0;
}

// ---- infinite Hover Boots
//
// How long the boots carry the player is in the player's game parameters (Player.game__player__PlayerGameParam):
// HoverBootsFloatingTime 84 and HoverBootsFloatingTimeAfterJump 42, frames, which the game keeps in memory as two floats
// in a row between 1.0 and 3.6 (seen in the game, 1.1.1; setting both higher there made the boots hover on). While the
// option is on both hold BOOTS_FOREVER, so the boots' own hover lasts until the player lands or takes them off; when it
// is switched off they are 84 and 42 again, which also ends a hover that has run longer. A helper thread finds the
// block in the heap; every frame checks it is still there and finds it again when it is not (a new player, a reload).
#define BOOTS_TIME 84.0f
#define BOOTS_TIME_JUMP 42.0f
#define BOOTS_FOREVER 1e6f        // frames: hours
#define BOOTS_BEFORE 1.0f         // the parameter in memory before the two times
#define BOOTS_AFTER 3.6f          // and after them
#define BOOTS_RETRY_NS 5000000000ull   // a search that found nothing waits this long before the next
#define HEAP_FROM 0x100000000ull       // the game's heap, as in debug_warp.c dbg_each_actor
#define HEAP_TO 0x700000000ull

static _Atomic uint64_t boots_at;   // the address of HoverBootsFloatingTime, 0 while not known
static _Atomic int boots_looking;
static _Atomic uint64_t boots_tried_ns;

static int boots_block(const float v[4], int forever) {
    return v[0] == BOOTS_BEFORE && v[3] == BOOTS_AFTER &&
           (forever ? v[1] == BOOTS_FOREVER && v[2] == BOOTS_FOREVER : v[1] == BOOTS_TIME && v[2] == BOOTS_TIME_JUMP);
}

static void boots_thread(void *arg) {
    (void)arg;
    enum { CHUNK = 1 << 20 };
    static float buf[CHUNK / 4 + 4];
    uint64_t found = 0;
    for (uint64_t addr = HEAP_FROM; addr < HEAP_TO && !found;) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, HEAP_TO, &end);
        if (end <= addr) break;
        for (uint64_t at = addr; st == PLAT_PROT_RW && at < end && !found; at += CHUNK) {
            uint64_t len = end - at < CHUNK + 12 ? end - at : CHUNK + 12;   // 12 more bytes: a block on the seam
            if (len < 16 || !plat_mem_read(at, buf, len)) continue;
            for (uint64_t i = 0; i + 4 <= len / 4 && i < CHUNK / 4; i++)
                if ((buf[i + 1] == BOOTS_TIME || buf[i + 1] == BOOTS_FOREVER) && (boots_block(buf + i, 0) || boots_block(buf + i, 1))) {
                    found = at + i * 4 + 4;
                    break;
                }
        }
        addr = end;
    }
    if (found) atomic_store(&boots_at, found);
    atomic_store(&boots_tried_ns, plat_now_ns());
    atomic_store(&boots_looking, 0);
}

static void boots_frame(void) {
    int on = dbg_hover_boots_on();
    uint64_t at = atomic_load(&boots_at);
    float v[4];
    if (at && !(plat_mem_read(at - 4, v, sizeof v) && (boots_block(v, 0) || boots_block(v, 1)))) atomic_store(&boots_at, 0), at = 0;
    if (!at) {
        // Nothing to put back while off: a block found again holds the game's own times.
        int zero = 0;
        if (on && plat_now_ns() - atomic_load(&boots_tried_ns) > BOOTS_RETRY_NS &&
            atomic_compare_exchange_strong(&boots_looking, &zero, 1) && !plat_thread_spawn(boots_thread, NULL, "debug-boots", 0))
            atomic_store(&boots_looking, 0);
        return;
    }
    if (boots_block(v, on)) return;
    float t = on ? BOOTS_FOREVER : BOOTS_TIME, tj = on ? BOOTS_FOREVER : BOOTS_TIME_JUMP;
    uint32_t bt, btj;
    memcpy(&bt, &t, 4), memcpy(&btj, &tj, 4);
    dbg_write32(at, bt), dbg_write32(at + 4, btj);
}

// ---- PastyPixel's Bomb T-Pose
//
// The bug in the game (1.1.1; docs/bomb-t-pose.md has the whole account): the player's animation (the AS component,
// "?AS/Player_Adult.root.asb" or "?AS/Player_Child.root.asb", whose owner is at component+0x70) blends a list of layers,
// each through a bone mask. The face layer (its states are Face..., and DemoOcarina... while the ocarina plays)
// normally masks the face bones only. Played until the notes change, the ocarina's face state DemoOcarinaBreath gives
// that layer the breathing mask: the face, the chest and both arms. The face states that come after it (FaceDefault,
// FaceLocomotion) name no mask of their own, so the breathing mask stays until a state that does name one (a bomb
// pulled out standing still: its WeaponEquipOn sets the face mask again; standing idle a while does too). A bomb pulled
// out while moving moves the face layer to the body's Move node, which has no face animation: the layer fades into
// having no state, and stays so until FaceDefault starts (about 16 frames). For those frames the bones under the
// breathing mask take the stateless layer's pose instead of the carry's: both arms out to the sides, the bomb in the
// right hand held out at shoulder height. FaceDefault fading in brings the arms back up over the head.
//
// While the option is on and the player lifts or carries a bomb (the lifter's state 2 or 3 and the blackboard's
// LiftableType 4), dbg_bomb_layers, called from the animation owner's update after it has updated its layers
// (debug_game.c debug_bomb_layers), gives the face layer the breathing mask's definition (mask_define: the bits are
// built from it as the game builds them), so every pull while moving goes as it does after the ocarina. The face
// states play as usual. When the carry ends (thrown, put away, set down) or the option goes off, the definition the
// layer had goes back the same way.
#define ACTOR_COMPS 0x270        // the actor's component array, their number at +0x278
#define ACTOR_COMP_COUNT 0x278
#define COMP_NAME 0x8            // a component's resource name
#define AS_OWNER 0x70            // the AS component's animation owner
#define OWNER_BB 0x58            // the owner's parameter store (blackboard)
#define OWNER_LAYERS 0x198       // the owner's layers, LAYER_STEP apart
#define LAYER_STEP 0x48
#define LAYER_MAX 12
#define LAYER_CLASS 0x4c29788    // a layer's vtable, main+
#define LAYER_MASK 0x50          // the layer's bone mask (MASK_*)
#define LAYER_STATE_NAMES 0x250  // the names of the layer's two state slots
// A layer's mask: its bits (from the pointer at +0x10, 0x80 bytes) are built from a definition that follows them (the
// pointer at +0x18; its method at vtable +0x30 builds them, given the mask's +0x8). The definition lists bones, each
// with a flag byte: the bone and the bones under it are set or cleared, in order (DEF_*).
#define MASK_SET 0x8
#define MASK_DEF 0x18
#define MASK_FLAGS 0x30          // bit 0: the definition changed, bits not built yet; bit 4: built, not yet taken up
#define DEF_BUILD 0x30           // the definition's method that builds the bits
#define DEF_FLAGS 0x28           // the flag bytes (pointer), their room at +0x20
#define DEF_VERSION 0x30         // counts the definitions the mask has been given
#define DEF_COUNT 0x34           // u16: the number of bones listed
#define DEF_BONES 0x40           // the bones (pointer; each an s16 skeleton number and an s16 bone), their room at +0x38
#define DEF_ROOM 16
#define LIST_COUNT 0x18          // the owner's layer list: the number of layers, their entries (LAYER_STEP apart)
#define LIST_LAYERS 0x20
#define LIFTER_STATE 0x2b0       // 0 nothing, 2 lifting, 3 carrying
#define BB_INTS 0x18             // the blackboard's ints: their number, then the values from +8
#define BB_LIFTABLE_TYPE 14      // LiftableType: 4 while a bomb is lifted or carried, 8 with nothing (both ages)
#define LIFTABLE_BOMB 4

// The breathing mask's definition as the game gives it with DemoOcarinaBreath (seen in the game, 1.1.1, both ages):
// Root and the bones under it, then Spine_2 (the chest, both arms, the head) the other way, then the bones that hold
// things (Weapon_L, Weapon_R) and the fingers back as Root. The face mask the game uses otherwise lists Root and
// Face_Root (bone 107 at both ages) the same way.
#define BREATH_BONES 14
static const uint8_t breath_flags[BREATH_BONES] = {0x83, 0x80, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83, 0x83};
static const int16_t breath_child[BREATH_BONES] = {0, 11, 23, 68, 20, 24, 29, 34, 39, 65, 69, 74, 79, 84};
static const int16_t breath_adult[BREATH_BONES] = {0, 6, 18, 66, 14, 19, 24, 29, 34, 62, 67, 72, 77, 82};

static uint64_t rd64(uint64_t at) {
    uint64_t v = 0;
    return plat_mem_read(at, &v, 8) ? v : 0;
}

static int comp_named(uint64_t comp, const char *part, char *name, size_t n) {
    uint64_t p = rd64(comp + COMP_NAME);
    if (!p || !plat_mem_read(p, name, n - 1)) return 0;
    name[n - 1] = 0;
    return strstr(name, part) != NULL;
}

// Found by bomb_frame on every frame (debug frame hook) and read by dbg_bomb_layers (the animation owner's update):
// the face layer (0: none known, or the option off), the player's age, and where the lifter's state and the
// blackboard's LiftableType are.
static _Atomic uint64_t bomb_face, bomb_state_at, bomb_liftable_at;
static _Atomic int bomb_child, bomb_holding;   // bomb_holding: dbg_bomb_layers holds a mask, to be put back
static struct {
    uint64_t actor, comps, as, lifter;   // the player, its component array, its AS and lifter components
} bomb;
// Used by dbg_bomb_layers only: the layer given the breathing mask, and the definition it had before.
static struct {
    uint64_t layer;
    uint16_t count;
    uint8_t flags[DEF_ROOM];
    int16_t bones[DEF_ROOM][2];
} held;

static void bomb_components(uint64_t actor) {
    char name[96];
    uint64_t arr = rd64(actor + ACTOR_COMPS);
    uint32_t n = 0;
    bomb.actor = actor, bomb.comps = arr, bomb.as = bomb.lifter = 0;
    if (!arr || !plat_mem_read(actor + ACTOR_COMP_COUNT, &n, 4) || n > 1024) return;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = rd64(arr + 8ull * i);
        if (!c) continue;
        if (!bomb.as && comp_named(c, ".root.asb", name, sizeof name)) atomic_store(&bomb_child, strstr(name, "_Child") != NULL), bomb.as = c;
        else if (!bomb.lifter && comp_named(c, "PlayerLifter", name, sizeof name)) bomb.lifter = c;
    }
}

// The face layer: the one whose state slots are named Face... or DemoOcarina... (the names stay while it has no state).
static uint64_t face_layer(uint64_t owner) {
    for (int i = 0; i < LAYER_MAX; i++) {
        uint64_t l = rd64(owner + OWNER_LAYERS + (uint64_t)i * LAYER_STEP);
        if (!l || rd64(l) != MOD_MAIN_BASE + LAYER_CLASS) continue;
        for (int s = 0; s < 2; s++) {
            char nm[12] = "";
            uint64_t p = rd64(l + LAYER_STATE_NAMES + 8ull * s);
            if (p && plat_mem_read(p, nm, sizeof nm - 1) && (!strncmp(nm, "Face", 4) || !strncmp(nm, "DemoOcarina", 11))) return l;
        }
    }
    return 0;
}

static void bomb_forget(void) {
    atomic_store(&bomb_face, 0);
    atomic_store(&bomb_state_at, 0);
    atomic_store(&bomb_liftable_at, 0);
}

static void bomb_frame(void) {
    uint64_t actor = dbg_player_actor();
    if ((!dbg_bomb_bug_on() && !atomic_load(&bomb_holding)) || !actor) {
        bomb_forget();
        return;
    }
    if (actor != bomb.actor || rd64(actor + ACTOR_COMPS) != bomb.comps || !bomb.as || !bomb.lifter) {
        bomb_forget();   // a new player (a reload, an age switch)
        bomb_components(actor);
        if (!bomb.as || !bomb.lifter) return;
    }
    uint64_t owner = rd64(bomb.as + AS_OWNER), bb = owner ? rd64(owner + OWNER_BB) : 0, ints = bb ? rd64(bb + BB_INTS) : 0;
    uint64_t layer = owner ? face_layer(owner) : 0;
    uint32_t nints = 0;
    if (!ints || !plat_mem_read(ints, &nints, 4) || nints <= BB_LIFTABLE_TYPE || !layer) {
        bomb_forget();
        return;
    }
    atomic_store(&bomb_state_at, bomb.lifter + LIFTER_STATE);
    atomic_store(&bomb_liftable_at, ints + 8 + 4 * BB_LIFTABLE_TYPE);
    atomic_store(&bomb_face, layer);
}

// A bomb lifted or carried right now, read where bomb_frame found the lifter's state and LiftableType.
static int bomb_carrying(void) {
    uint64_t sa = atomic_load(&bomb_state_at), la = atomic_load(&bomb_liftable_at);
    uint32_t state = 0;
    int32_t liftable = 0;
    return sa && la && plat_mem_read(sa, &state, 4) && plat_mem_read(la, &liftable, 4) && (state == 2 || state == 3) &&
           liftable == LIFTABLE_BOMB;
}

// Gives a layer's mask this definition, as the game does: the version moves on, then the bits are built and marked built
// (the play routine takes them up from there).
static void mask_define(struct Cpu *c, uint64_t layer, uint16_t count, const uint8_t *flags, const int16_t (*bones)[2]) {
    uint64_t m = LD64(layer + LAYER_MASK), d = LD64(m + MASK_DEF), fl = LD64(d + DEF_FLAGS), bo = LD64(d + DEF_BONES);
    for (uint16_t i = 0; i < count; i++) {
        ST8(fl + i, flags[i]);
        ST16(bo + 4ull * i, (uint16_t)bones[i][0]);
        ST16(bo + 4ull * i + 2, (uint16_t)bones[i][1]);
    }
    ST16(d + DEF_COUNT, count);
    ST32(d + DEF_VERSION, LD32(d + DEF_VERSION) + 1);
    rt_call(c, LD64(LD64(d) + DEF_BUILD), 2, d, m + MASK_SET);
    ST8(m + MASK_FLAGS, (uint8_t)((LD8(m + MASK_FLAGS) & ~1u) | 0x10));
}

// Whether a layer's mask has the breathing definition; when it has not, its definition is kept in held.
static int mask_is_breath(uint64_t layer, const int16_t *breath, int keep) {
    uint64_t m = LD64(layer + LAYER_MASK), d = m ? LD64(m + MASK_DEF) : 0;
    if (!d || LD32(d + 0x20) < DEF_ROOM || LD32(d + 0x38) < DEF_ROOM) return -1;   // not the layout known
    uint64_t fl = LD64(d + DEF_FLAGS), bo = LD64(d + DEF_BONES);
    uint16_t n = LD16(d + DEF_COUNT);
    int same = n == BREATH_BONES;
    for (uint16_t i = 0; same && i < n; i++)
        same = LD8(fl + i) == breath_flags[i] && LD16(bo + 4ull * i) == 0 && (int16_t)LD16(bo + 4ull * i + 2) == breath[i];
    if (!same && keep && n <= DEF_ROOM) {
        held.count = n;
        for (uint16_t i = 0; i < n; i++)
            held.flags[i] = LD8(fl + i), held.bones[i][0] = (int16_t)LD16(bo + 4ull * i), held.bones[i][1] = (int16_t)LD16(bo + 4ull * i + 2);
    }
    return same;
}

void dbg_bomb_layers(struct Cpu *c, uint64_t list) {
    uint64_t face = atomic_load(&bomb_face), arr = LD64(list + LIST_LAYERS);
    uint32_t n = LD32(list + LIST_COUNT), i = 0;
    while (face && arr && i < n && i < LAYER_MAX && LD64(arr + LAYER_STEP * (uint64_t)i) != face) i++;
    if (!face || !arr || i >= n || i >= LAYER_MAX) {
        // Not this owner's layers, or no face layer known: a held layer that has gone (a new player) is forgotten
        // unwritten, as its memory may be in other use by now.
        if (held.layer && held.layer != face) held.layer = 0, atomic_store(&bomb_holding, 0);
        return;
    }
    const int16_t *breath = atomic_load(&bomb_child) ? breath_child : breath_adult;
    if (!atomic_load(&bomb_bug_on) || !bomb_carrying()) {
        // The carry is over: the definition the layer had goes back, unless the game has given it another since.
        if (held.layer == face && mask_is_breath(face, breath, 0) == 1) mask_define(c, face, held.count, held.flags, held.bones);
        held.layer = 0;
        atomic_store(&bomb_holding, 0);
        return;
    }
    int is = mask_is_breath(face, breath, 1);
    if (is < 0) return;
    if (!is) {
        // A definition of the game's own (the one the carry started with, or one a state gave since) is kept in held to
        // go back at the end; the breathing one takes its place.
        int16_t bones[BREATH_BONES][2];
        for (int k = 0; k < BREATH_BONES; k++) bones[k][0] = 0, bones[k][1] = breath[k];
        mask_define(c, face, BREATH_BONES, breath_flags, (const int16_t (*)[2])bones);
        held.layer = face;
        atomic_store(&bomb_holding, 1);
    }
}

// ---- per frame

static uint64_t last_ns;
static float air_pos[3], fwd[3] = {0, 0, 1};
static int air_mode;   // which of FLY, HOVER owns air_pos (0: none)
enum { AIR_FLY = 1, AIR_HOVER };
#define WALK_MS 6.0f   // hover moves at about the player's running speed

static float axis(int32_t v) {
    float f = (float)v / 32767.0f;
    return fabsf(f) < STICK_DEAD ? 0.0f : f;
}

// In the air the game gives the player no speed of its own, so both modes move air_pos with the left stick: up goes
// where the camera looks, right to its right, A held three times faster. Fly also rises on X and sinks on B.
static void air_step(struct Cpu *c, const float p[3], uint64_t b, int32_t lx, int32_t ly, float speed, int vertical, float dt) {
    float cam[3];
    if (camera_pos(cam)) {
        float dx = p[0] - cam[0], dz = p[2] - cam[2], n = sqrtf(dx * dx + dz * dz);
        if (n > 0.05f) fwd[0] = dx / n, fwd[2] = dz / n;
    }
    float sx = axis(lx), sy = axis(ly);
    if (b & BTN_A) speed *= 3.0f;
    float move[3] = {fwd[0] * sy - fwd[2] * sx, 0, fwd[2] * sy + fwd[0] * sx};
    air_pos[0] += move[0] * speed * dt;
    air_pos[2] += move[2] * speed * dt;
    if (vertical) air_pos[1] += (((b & BTN_X) ? 1.0f : 0.0f) - ((b & BTN_B) ? 1.0f : 0.0f)) * speed * dt;
    dbg_frame_teleport(c, air_pos, (sx != 0 || sy != 0) ? move : NULL);
}

void dbg_move_frame(struct Cpu *c) {
    uint64_t now = plat_now_ns();
    float dt = last_ns ? (float)(now - last_ns) * 1e-9f : 0.0f;
    last_ns = now;
    if (dt > FRAME_DT_MAX) dt = 0.0f;
    boots_frame();
    bomb_frame();
    float p[3];
    if (!dbg_player_pos(p)) {
        air_mode = 0;
        return;
    }

    // Which mode holds the player this frame: fly wins over hover.
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int mode = atomic_load(&fly_on) ? AIR_FLY : atomic_load(&hover_on) ? AIR_HOVER : 0;
    if (!mode) {
        air_mode = 0;
        return;
    }
    if (mode != air_mode) memcpy(air_pos, p, sizeof air_pos), air_mode = mode;
    input_poll_ui(&b, &lx, &ly, &rx, &ry);
    air_step(c, p, b, lx, ly, mode == AIR_FLY ? dbg_fly_speed() : WALK_MS, mode == AIR_FLY, dt);
}
