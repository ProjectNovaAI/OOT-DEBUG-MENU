// Fly, hover and infinite Hover Boots for the debug menu (debug_tools.h). Game build 1.1.1 only.
//
// All three put the player where they want with the game's own actor warp (debug_warp.c dbg_frame_teleport) on every
// frame, from the debug frame hook (debug_game.c, game thread), so the physics body goes along and gravity never gets to
// act. Each keeps its own position and moves it with the left stick: fly at its own speed and also up and down, hover
// and the boots at a fixed height and about running speed (the game gives a player in the air no walking speed).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "debug_tools.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

#define BTN_A (1ull << 0)
#define BTN_B (1ull << 1)
#define BTN_X (1ull << 2)
#define STICK_DEAD 0.15f
#define FRAME_DT_MAX 0.1f           // seconds: a longer gap (a loading screen) moves nothing
#define BOOTS_FALL_MS 1.5f          // falling faster than this (m/s) with the Hover Boots on starts the hold
#define CAMERA_NAME "PlayerCamera"  // its position is the eye: the player minus it is the view direction

static _Atomic int fly_on, hover_on, boots_on;
static _Atomic float fly_speed = 8.0f;

void dbg_fly(int on) { atomic_store(&fly_on, on != 0); }
int dbg_fly_on(void) { return atomic_load(&fly_on); }
void dbg_hover(int on) { atomic_store(&hover_on, on != 0); }
int dbg_hover_on(void) { return atomic_load(&hover_on); }
void dbg_hover_boots(int on) { atomic_store(&boots_on, on != 0); }
int dbg_hover_boots_on(void) { return atomic_load(&boots_on); }
float dbg_fly_speed(void) { return atomic_load(&fly_speed); }
void dbg_set_fly_speed(float ms) { atomic_store(&fly_speed, ms < 1.0f ? 1.0f : ms > 100.0f ? 100.0f : ms); }

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

// ---- per frame

static uint64_t last_ns;
static float air_pos[3], fwd[3] = {0, 0, 1}, last_y;
static int air_mode, last_have;   // air_mode: which of FLY, HOVER, BOOTS owns air_pos (0: none)
static uint32_t k_boot;
enum { AIR_FLY = 1, AIR_HOVER, AIR_BOOTS };
#define WALK_MS 6.0f   // hover and the boots move at about the player's running speed

static int hover_boots_worn(void) {
    char boot[64];
    if (!k_boot) k_boot = dbg_flag_key("PlayerStatus.EquipBoot");
    return dbg_flag_get_string(k_boot, boot) && !strcmp(boot, "Armor_Adult_HoverBoot");
}

static float axis(int32_t v) {
    float f = (float)v / 32767.0f;
    return fabsf(f) < STICK_DEAD ? 0.0f : f;
}

// In the air the game gives the player no speed of its own, so every mode moves air_pos with the left stick: up goes
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
    float p[3];
    if (!dbg_player_pos(p)) {
        air_mode = last_have = 0;
        return;
    }
    float vy = last_have && dt > 0 ? (p[1] - last_y) / dt : 0.0f;
    last_y = p[1], last_have = 1;

    // Which mode holds the player this frame. Infinite Hover Boots: with the boots on, a fall (faster than
    // BOOTS_FALL_MS) starts a hold at the height it started from, until the boots come off or B is pressed.
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int mode = 0;
    if (atomic_load(&fly_on)) mode = AIR_FLY;
    else if (atomic_load(&hover_on)) mode = AIR_HOVER;
    else if (atomic_load(&boots_on) && hover_boots_worn()) {
        input_poll_ui(&b, &lx, &ly, &rx, &ry);
        if (air_mode == AIR_BOOTS && !(b & BTN_B)) mode = AIR_BOOTS;
        else if (!(b & BTN_B) && vy < -BOOTS_FALL_MS) {
            mode = AIR_BOOTS;
            p[1] -= vy * dt;   // back to where the fall started
        }
    }
    if (!mode) {
        air_mode = 0;
        return;
    }
    if (mode != air_mode) memcpy(air_pos, p, sizeof air_pos), air_mode = mode;
    if (mode != AIR_BOOTS) input_poll_ui(&b, &lx, &ly, &rx, &ry);
    air_step(c, p, b, lx, ly, mode == AIR_FLY ? atomic_load(&fly_speed) : WALK_MS, mode == AIR_FLY, dt);
}
