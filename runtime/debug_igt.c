// Speedrun timer for the debug menu (debug_tools.h). Game build 1.1.1 only.
//
// In-game time (IGT) is real time with the loading screens taken out: it stops while Sequence_IsLoadingBanc is set or
// there is no player actor (the title screen, the moments of an area change). RTA, real time, runs alongside. Splits
// are taken by hand or, with auto-split, on every area change (Sequence_CurrentBanc), labelled with the area that was
// left. The best run's splits are kept in debug_splits.txt in the game's folder and every split shows its difference
// to the same split of that run. Ticked once per presented frame (dbg_tick).
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "platform/platform.h"

#define BEST_FILE "debug_splits.txt"
#define TICK_GAP_NS 250000000ull   // a longer gap between presents (a stall, the window dragged) counts no time

static PlatMutex lock;   // the state below: written by the present thread and the menu
static int running, have_best, best_n, split_n, was_loading;
static uint64_t igt_ns, rta_ns, last_ns;
static uint64_t split_ns[DBG_SPLITS], best_ns[DBG_SPLITS];
static char split_name[DBG_SPLITS][48], last_area[64];
static uint32_t k_loading, k_area;

int dbg_igt_enabled(void) { return cfg_bool("NX2NAVI_DEBUG_IGT"); }
int dbg_igt_autosplit(void) { return cfg_bool("NX2NAVI_DEBUG_IGT_AUTOSPLIT"); }
int dbg_igt_autostart(void) { return cfg_bool("NX2NAVI_DEBUG_IGT_AUTOSTART"); }

static void load_best(void) {
    uint64_t size = 0;
    char *text = (char *)plat_file_read(BEST_FILE, &size);
    best_n = 0;
    if (text) {
        char *end = text + size, *p = text;
        while (p < end && best_n < DBG_SPLITS) {
            unsigned long long v = strtoull(p, &p, 10);
            if (v) best_ns[best_n++] = v;
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
        }
        free(text);
    }
    have_best = 1;
}

// One line a split: the IGT in nanoseconds, then the label.
static void save_best(void) {
    char buf[DBG_SPLITS * 80];
    size_t n = 0;
    for (int i = 0; i < split_n && n < sizeof buf - 80; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%llu %s\n", (unsigned long long)split_ns[i], split_name[i]);
    plat_file_write(BEST_FILE, buf, n);
    memcpy(best_ns, split_ns, sizeof best_ns);
    best_n = split_n;
}

static void add_split(const char *name) {
    if (split_n >= DBG_SPLITS) return;
    split_ns[split_n] = igt_ns;
    snprintf(split_name[split_n], sizeof split_name[0], "%s", name && *name ? name : "Split");
    split_n++;
}

void dbg_igt_tick(uint64_t now_ns) {
    if (!k_loading) k_loading = dbg_flag_key("Sequence_IsLoadingBanc"), k_area = dbg_flag_key("Sequence_CurrentBanc");
    int loading = dbg_flag_get_bool(k_loading) == 1 || !dbg_player_actor();
    char area[64] = "";
    int have_area = !loading && dbg_current_area(area) && area[0];
    plat_mutex_lock(&lock);
    if (!have_best) load_best();
    uint64_t dt = last_ns && now_ns > last_ns && now_ns - last_ns < TICK_GAP_NS ? now_ns - last_ns : 0;
    last_ns = now_ns;
    // Auto-start: the moment a save's player first appears after a stretch without one.
    if (!running && !split_n && !igt_ns && was_loading && !loading && dbg_igt_autostart() && dbg_igt_enabled()) running = 1;
    was_loading = loading;
    if (running) {
        rta_ns += dt;
        if (!loading) igt_ns += dt;
    }
    if (have_area) {
        if (running && last_area[0] && strcmp(area, last_area) && dbg_igt_autosplit()) add_split(last_area);
        snprintf(last_area, sizeof last_area, "%s", area);
    }
    plat_mutex_unlock(&lock);
}

void dbg_igt_toggle(void) {
    plat_mutex_lock(&lock);
    running = !running;
    plat_mutex_unlock(&lock);
}

int dbg_igt_running(void) { return running; }

void dbg_igt_split(void) {
    plat_mutex_lock(&lock);
    if (running) add_split(last_area);
    plat_mutex_unlock(&lock);
}

// Reset: a finished run (stopped, with splits) that beats the best run's last split, or any run when there is no
// best, becomes the best.
int dbg_igt_reset(void) {
    int better = 0;
    plat_mutex_lock(&lock);
    if (!running && split_n && (!best_n || split_ns[split_n - 1] < best_ns[best_n - 1] || split_n > best_n)) save_best(), better = 1;
    running = 0;
    igt_ns = rta_ns = 0;
    split_n = 0;
    plat_mutex_unlock(&lock);
    return better;
}

void dbg_igt_times(uint64_t *igt, uint64_t *rta) {
    plat_mutex_lock(&lock);
    if (igt) *igt = igt_ns;
    if (rta) *rta = rta_ns;
    plat_mutex_unlock(&lock);
}

int dbg_igt_splits(void) { return split_n; }

// Split i: its IGT, its label, and the difference to the best run's split i (0 without one; *has_delta says).
int dbg_igt_split_at(int i, uint64_t *ns, char name[48], int64_t *delta, int *has_delta) {
    plat_mutex_lock(&lock);
    int ok = i >= 0 && i < split_n;
    if (ok) {
        if (ns) *ns = split_ns[i];
        if (name) memcpy(name, split_name[i], 48);
        int hd = i < best_n;
        if (has_delta) *has_delta = hd;
        if (delta) *delta = hd ? (int64_t)split_ns[i] - (int64_t)best_ns[i] : 0;
    }
    plat_mutex_unlock(&lock);
    return ok;
}

int dbg_igt_best_count(void) { return best_n; }

void dbg_igt_clear_best(void) {
    plat_mutex_lock(&lock);
    best_n = 0;
    plat_file_write(BEST_FILE, "", 0);
    plat_mutex_unlock(&lock);
}
