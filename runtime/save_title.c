// The profile picker on the game's title screen (save_slots.h): while the title menu or its Load list is up, the game's
// R button (RB, or the keys bound to it) switches to the next profile and L (LB) to the one before, round the sorted
// list, while the game runs. The profile shows at once (ui/debug_tab.cpp debug_overlay_draw).
//
// A switch puts the game's save into the profile in use and makes the other profile's save the game's save at once
// (save_profile_switch_now: the committed save and its working copy), then has the game read its save list again
// (dbg_save_list_reload), so the Load list shows that profile's saves and loading one loads it. That is safe here
// because on the title the game holds nothing of its save that it would write back: it reads a slot's progress when
// the slot is loaded. While a switch runs the game gets neither A nor B, so no slot loads halfway through it. When the
// working copy cannot be swapped (a file in it held open), the game restarts into the new profile instead.
//
// The full-screen UI enum identifies Title, its Load page, and the in-game Save screen.
// Unlike the debug title flag and last area/player pointers, it changes when returning from gameplay.
// On Save, L/R choose a destination without importing its progress; A prepares save_profile_adopt
// and is forwarded to the game only after that profile becomes active.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "platform/platform.h"
#include "save_slots.h"

#define TITLE_POLL_MS 100
#define FAIL_SHOW_MS 4000
#define LIST_WAIT_MS 10000

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static struct {
    char names[SAVE_LIST_MAX][SAVE_NAME_MAX];
    int has_save[SAVE_LIST_MAX];
    int confirmed;
    int saving, save_press;
    int count, active, chosen;   // indexes into names
    int loaded;                  // the list is read (once per title visit)
    int phase;                   // SAVE_TITLE_
    uint64_t failed_ns;
} T;

static volatile int g_at_title;

// g_lock held, or a glance from the render thread
static int busy(void) {
    return T.phase == SAVE_TITLE_SWITCHING || T.phase == SAVE_TITLE_RELOADING || T.phase == SAVE_TITLE_RESTART ||
           T.phase == SAVE_TITLE_RESTARTING;
}

static int at_title(void) {
    static uint64_t at;
    static int last;
    uint64_t now = plat_now_ns();
    if (at && now - at < TITLE_POLL_MS * 1000000ull) return last;
    at = now;
    last = 0;
    if (!cfg_bool("NX2NAVI_DEBUG_TITLE_PROFILES") || !dbg_available()) return 0;
    // The title and ConfirmationLoad sequences set this enum to Title and Load. Keep
    // their context through the title's Option page; entering gameplay or a game menu
    // clears it, so the in-game Load page cannot switch a running session's save.
    static int title_context;
    uint32_t ui = 0;
    if (!dbg_flag_get_u32(DBG_FLAG_ENUM, 0x578d41e1u, &ui)) return 0;
    if (ui == 0x43080996u) title_context = 1;   // Title
    else if (ui != 0x3b849e33u && ui != 0xa60ba395u) title_context = 0;   // Load, Option
    return last = title_context && (ui == 0x43080996u || ui == 0x3b849e33u);
}

// Sequence/Save.module.ainb uses the same full-screen enum as the title. A stale player
// or area pointer is irrelevant: only the game's Save screen exposes this destination picker.
static int at_save(void) {
    uint32_t ui = 0;
    return dbg_available() && dbg_flag_get_u32(DBG_FLAG_ENUM, 0x578d41e1u, &ui) && ui == 0x42814d01u;
}

// The profiles and the one in use. g_lock held.
static void load_list(void) {
    char active[SAVE_NAME_MAX] = "";
    T.count = save_profile_list(T.names, SAVE_LIST_MAX);
    T.active = T.chosen = 0;
    if (save_profile_active(active))
        for (int i = 0; i < T.count; i++)
            if (!strcmp(T.names[i], active)) T.active = T.chosen = i;
    if (!T.count) snprintf(T.names[0], SAVE_NAME_MAX, "%s", active), T.count = 1;
    for (int i = 0; i < T.count; i++) T.has_save[i] = save_profile_has_save(T.names[i]);
    T.loaded = 1;
}

// g_lock held.
static void failed(void) {
    T.phase = SAVE_TITLE_FAILED, T.failed_ns = plat_now_ns();
    T.chosen = T.active;
}

// The game reads its save list again; 0 once it has.
static int reload_list(void) {
    uint64_t t0 = plat_now_ns();
    int queued = 0;
    while (plat_now_ns() - t0 < LIST_WAIT_MS * 1000000ull) {
        if (!queued) queued = dbg_save_list_reload();
        else {
            int s = dbg_save_list_state();
            if (s > 0) return 0;
            if (s < 0) queued = 0;   // the list was still reading: ask again
        }
        plat_sleep_ms(20);
    }
    return -1;
}

// Switches until the profile in use is the one chosen (L and R may move the choice while a switch runs).
static void switch_thread(void *arg) {
    (void)arg;
    for (;;) {
        char name[SAVE_NAME_MAX];
        plat_mutex_lock(&g_lock);
        if (T.chosen == T.active) {
            T.phase = SAVE_TITLE_IDLE;
            plat_mutex_unlock(&g_lock);
            return;
        }
        if (!T.saving && (!T.has_save[T.active] || !T.has_save[T.chosen]) && !T.confirmed) {
            T.phase = SAVE_TITLE_CHOSEN;
            plat_mutex_unlock(&g_lock);
            return;
        }
        int saving = T.saving;
        T.confirmed = 0;
        snprintf(name, sizeof name, "%s", T.names[T.chosen]);
        T.phase = SAVE_TITLE_SWITCHING;
        plat_mutex_unlock(&g_lock);
        // the game may still be writing its save (it saves its options on the title): the switch waits for that
        for (int i = 0; i < 100 && fs_save_busy(); i++) plat_sleep_ms(50);
        int r = saving ? save_profile_adopt(name) : save_profile_switch_now(name);
        plat_mutex_lock(&g_lock);
        if (r < 0) {
            failed();
            plat_mutex_unlock(&g_lock);
            return;
        }
        char pending[SAVE_NAME_MAX];
        snprintf(pending, sizeof pending, "%s", T.names[T.chosen]);
        load_list();   // the profile in use is name now
        for (int i = 0; i < T.count; i++)
            if (!strcmp(T.names[i], pending)) T.chosen = i;
        if (r > 0) {
            T.phase = SAVE_TITLE_RESTART;
            plat_mutex_unlock(&g_lock);
            return;
        }
        if (saving) {
            T.phase = SAVE_TITLE_IDLE;
            T.save_press = 1;
            plat_mutex_unlock(&g_lock);
            return;
        }
        T.phase = SAVE_TITLE_RELOADING;
        plat_mutex_unlock(&g_lock);
        if (reload_list()) {
            // the files are switched but the game's list is not: a restart shows the right one
            rt_log("title profiles: the game did not read its save list again; it restarts into %s", name);
            plat_mutex_lock(&g_lock);
            T.phase = SAVE_TITLE_RESTART;
            plat_mutex_unlock(&g_lock);
            return;
        }
        rt_log("title profiles: %s is in use; the Load list shows its saves", name);
    }
}

// g_lock held.
static void start_switch(void) {
    if (busy()) return;   // the running switch goes on to the new choice
    T.phase = SAVE_TITLE_SWITCHING;
    if (!plat_thread_spawn(switch_thread, NULL, "title-profile", 0)) failed();
}

void save_title_input(uint64_t *buttons) {
    static uint64_t held;
    int saving = at_save();
    int title = at_title() || saving;
    g_at_title = title;
    uint64_t b = *buttons, down = b & ~held;
    held = b;
    plat_mutex_lock(&g_lock);
    if (!title) {
        // left the title (a slot loaded, or the game is starting): the list is read again next time
        if (!busy()) T.loaded = 0, T.phase = SAVE_TITLE_IDLE;
        plat_mutex_unlock(&g_lock);
        return;
    }
    if (!T.loaded || (!busy() && T.saving != saving)) {
        load_list();
        T.phase = SAVE_TITLE_IDLE;
        T.save_press = 0;
    }
    if (!busy()) T.saving = saving;
    if (!T.saving && T.phase == SAVE_TITLE_FAILED && plat_now_ns() - T.failed_ns > FAIL_SHOW_MS * 1000000ull)
        T.phase = SAVE_TITLE_IDLE;
    const uint64_t L = 1ull << B_L, R = 1ull << B_R, A = 1ull << B_A, B = 1ull << B_B;
    int restarting = T.phase == SAVE_TITLE_RESTART || T.phase == SAVE_TITLE_RESTARTING;
    if (!restarting && (!T.saving || !busy()) && T.count > 1 && (down & (L | R))) {
        T.chosen = (T.chosen + (down & R ? 1 : T.count - 1)) % T.count;
        if (T.phase == SAVE_TITLE_FAILED) T.phase = SAVE_TITLE_IDLE;
        T.confirmed = 0;
        if (!busy()) {
            if (T.chosen == T.active) T.phase = SAVE_TITLE_IDLE;
            else if (T.saving || !T.has_save[T.active] || !T.has_save[T.chosen]) T.phase = SAVE_TITLE_CHOSEN;
            else start_switch();
        }
    }
    int save_failed = T.saving && T.phase == SAVE_TITLE_FAILED;
    if (save_failed && (down & B)) T.phase = SAVE_TITLE_IDLE;
    int choosing = T.phase == SAVE_TITLE_CHOSEN;
    if (choosing && (down & B)) {
        T.chosen = T.active;
        T.phase = SAVE_TITLE_IDLE;
    } else if (choosing && (down & A)) {
        T.confirmed = 1;
        start_switch();
    }
    int hold = busy() || choosing || save_failed;
    int press = T.save_press && saving;
    T.save_press = 0;
    plat_mutex_unlock(&g_lock);
    // L and R are the picker's here; while a switch runs, A and B are too
    b &= ~(L | R);
    if (hold) b &= ~(A | B);
    if (press) b |= A;
    *buttons = b;
}

int save_title_view(SaveTitleView *v) {
    if (!g_at_title) {
        plat_mutex_lock(&g_lock);
        int keep = T.phase == SAVE_TITLE_RESTART || T.phase == SAVE_TITLE_RESTARTING;
        plat_mutex_unlock(&g_lock);
        if (!keep) return 0;
    }
    plat_mutex_lock(&g_lock);
    if (!T.loaded) {
        plat_mutex_unlock(&g_lock);
        return 0;
    }
    snprintf(v->name, sizeof v->name, "%s", T.names[T.chosen]);
    snprintf(v->active, sizeof v->active, "%s", T.names[T.active]);
    v->index = T.chosen, v->count = T.count, v->phase = T.phase;
    v->saving = T.saving;
    plat_mutex_unlock(&g_lock);
    return 1;
}

int save_title_showing(void) { return g_at_title || busy(); }

int save_title_restart_due(void) {
    plat_mutex_lock(&g_lock);
    int due = T.phase == SAVE_TITLE_RESTART && !fs_save_busy();
    if (due) T.phase = SAVE_TITLE_RESTARTING;
    plat_mutex_unlock(&g_lock);
    return due;
}
