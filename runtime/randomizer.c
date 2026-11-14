// The Randomizer tab's work: runs tools/randomizer.py, which reads the game's stage files and writes the Randomizer mod
// (mods/Randomizer), with the options of nx2navi.ini [randomizer] (the NX2NAVI_RANDO_* keys of cfg.c).
//
// The game indexes its mods' files once, at start (romfs.c), so a running game must not see a new seed's files: the
// tool runs with --pending and leaves them in mods/Randomizer/romfs.new and spoiler.new, and rando_apply_pending, called
// by mods_scan before it lists the mods folder, moves them into place at the next start. The tool runs detached
// (plat_spawn: no console, no handle) and reports through a status file in build-cache, which rando_tick reads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"
#include "randomizer.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define RUN_TIMEOUT_MS (15u * 60u * 1000u)   // a run takes about a minute; one quiet this long has died

const RandoOption rando_options[] = {
    {"NX2NAVI_RANDO_LOGIC", "--logic", {"glitchless", "none"}, 2},
    {"NX2NAVI_RANDO_CHESTS", "--chests", {"on", "off"}, 2},
    {"NX2NAVI_RANDO_DROPS", "--drops", {"off", "own_pool", "with_chests"}, 3},
    {"NX2NAVI_RANDO_MAPS", "--maps-compasses", {"own_dungeon", "vanilla"}, 2},
    {"NX2NAVI_RANDO_ENEMIES", "--enemies", {"off", "same_kind", "any_kind"}, 3},
    {"NX2NAVI_RANDO_ENTRANCES", "--entrances", {"off", "dungeons", "bosses", "overworld", "all"}, 5},
    {"NX2NAVI_RANDO_ENTRANCE_POOLS", "--entrance-pools", {"separate", "mixed"}, 2},
    {"NX2NAVI_RANDO_DUNGEON_AGES", "--dungeon-ages", {"same", "any"}, 2},
};
const int rando_noptions = (int)(sizeof rando_options / sizeof rando_options[0]);

static int state = RANDO_IDLE;
static char note[512];
static char status_path[PLAT_PATH_MAX + 64];
static uint64_t started_ms, polled_ms;

static int exe_dir(char *out, size_t cap) {   // the folder of the executable, with a separator at the end
    if (!plat_exe_path(out, cap)) return 0;
    char *cut = strrchr(out, '/'), *back = strrchr(out, '\\');
    if (back > cut) cut = back;
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}

int rando_option_index(int i) {
    const RandoOption *o = &rando_options[i];
    const char *v = cfg_get(o->key);
    for (int k = 0; v && k < o->nvalues; k++)
        if (!strcmp(v, o->values[k])) return k;
    return 0;   // values[0] is each option's default (cfg.c)
}

const char *rando_seed(void) {
    const char *s = cfg_get("NX2NAVI_RANDO_SEED");
    return s ? s : "";
}

void rando_new_seed(void) {
    uint8_t r[5] = {0};
    if (!plat_random(r, sizeof r)) {
        uint64_t t = plat_unix_time_ns();
        memcpy(r, &t, sizeof r);
    }
    uint64_t v = 0;
    for (int k = 0; k < 5; k++) v = v << 8 | r[k];
    char s[16];
    snprintf(s, sizeof s, "%010llu", (unsigned long long)(v % 10000000000ull));
    cfg_set("NX2NAVI_RANDO_SEED", s);
}

int rando_state(void) { return state; }
const char *rando_note(void) { return note; }

static int mod_path(char *out, size_t cap, const char *leaf) {
    mods_scan();
    const char *d = mods_dir();
    if (!d || !*d) return 0;
    snprintf(out, cap, "%s" SEP RANDO_MOD "%s%s", d, *leaf ? SEP : "", leaf);
    return 1;
}

int rando_mod_present(void) {
    char p[PLAT_PATH_MAX];
    return mod_path(p, sizeof p, "romfs") && plat_dir_exists(p);
}

int rando_spoiler_path(char *out, int cap) {
    // a seed made this session is in spoiler.new until the next start
    return (mod_path(out, (size_t)cap, "spoiler.new") && plat_file_exists(out)) ||
           (mod_path(out, (size_t)cap, "spoiler.txt") && plat_file_exists(out));
}

// ---- the next start: a finished seed's files into place

static int remove_cb(const char *name, int kind, uint64_t size, void *ctx);
static void remove_tree(const char *path) {
    if (plat_fs_kind(path) == PLAT_FS_DIR) {
        plat_fs_list(path, remove_cb, (void *)path);
        plat_fs_rmdir(path);
    } else {
        plat_fs_unlink(path);
    }
}
static int remove_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)kind, (void)size;
    char p[PLAT_PATH_MAX];
    snprintf(p, sizeof p, "%s" SEP "%s", (const char *)ctx, name);
    remove_tree(p);
    return 1;
}

void rando_apply_pending(const char *mods) {
    char next[PLAT_PATH_MAX], cur[PLAT_PATH_MAX], old[PLAT_PATH_MAX + 8];
    snprintf(next, sizeof next, "%s" SEP RANDO_MOD SEP "romfs.new", mods);
    if (!plat_dir_exists(next)) return;
    snprintf(cur, sizeof cur, "%s" SEP RANDO_MOD SEP "romfs", mods);
    snprintf(old, sizeof old, "%s.old", cur);
    remove_tree(old);   // a start that stopped half way
    if (plat_dir_exists(cur) && plat_fs_rename(cur, old, 1) != PLAT_FS_OK) {
        char why[256];
        plat_fs_busy_reason(cur, why, sizeof why);
        rt_log("randomizer: cannot move the last seed's files out of %s (error %d%s%s); the new seed waits", cur,
               plat_fs_os_error(), *why ? ": " : "", why);
        return;
    }
    if (plat_fs_rename(next, cur, 1) != PLAT_FS_OK) {
        rt_log("randomizer: cannot move %s into place (error %d)", next, plat_fs_os_error());
        if (plat_dir_exists(old)) plat_fs_rename(old, cur, 1);
        return;
    }
    remove_tree(old);
    char sp_new[PLAT_PATH_MAX], sp[PLAT_PATH_MAX];
    snprintf(sp_new, sizeof sp_new, "%s" SEP RANDO_MOD SEP "spoiler.new", mods);
    snprintf(sp, sizeof sp, "%s" SEP RANDO_MOD SEP "spoiler.txt", mods);
    if (plat_file_exists(sp_new)) plat_file_replace(sp_new, sp, 1);
    rt_log("randomizer: the new seed is in place (%s)", cur);
}

// ---- a run

// The Python that runs the tool: NX2NAVI_RANDO_PYTHON, else the setup's (the first line of build-cache/rebuild.txt,
// which the setup writes; installer/steps.py rebuild_command), else the launcher's beside the game or in its usual
// folder. pythonw where there is one, so no console window opens.
static int find_python(const char *dir, char *out, size_t cap) {
    const char *set = cfg_get("NX2NAVI_RANDO_PYTHON");
    if (set && *set) {
        snprintf(out, cap, "%s", set);
        return plat_file_exists(out);
    }
    char p[PLAT_PATH_MAX + 64];
    snprintf(p, sizeof p, "%sbuild-cache" SEP "rebuild.txt", dir);
    uint64_t n = 0;
    char *cmd = (char *)plat_file_read(p, &n);
    if (cmd) {
        cmd[strcspn(cmd, "\r\n")] = 0;
        snprintf(out, cap, "%s", cmd);
        free(cmd);
        if (plat_file_exists(out)) return 1;
    }
    char local[PLAT_PATH_MAX] = "";
    plat_getenv("LOCALAPPDATA", local, sizeof local);
    const char *tries[] = {"%s.." SEP "python" SEP "pythonw.exe", "%s.." SEP "python" SEP "python.exe"};
    for (int k = 0; k < 2; k++) {
        snprintf(out, cap, tries[k], dir);
        if (plat_file_exists(out)) return 1;
    }
    if (*local) {
        const char *leaf[] = {"pythonw.exe", "python.exe"};
        for (int k = 0; k < 2; k++) {
            snprintf(out, cap, "%s" SEP "Programs" SEP "OOT-DEBUG-MENU" SEP "python" SEP "%s", local, leaf[k]);
            if (plat_file_exists(out)) return 1;
        }
    }
    return 0;
}

// The tool: tools/randomizer.py beside the game (build_game.py copies it there), else in the launcher's current app.
static int find_tool(const char *dir, char *out, size_t cap) {
    snprintf(out, cap, "%stools" SEP "randomizer.py", dir);
    if (plat_file_exists(out)) return 1;
    char local[PLAT_PATH_MAX] = "", cur[PLAT_PATH_MAX + 64];
    if (!plat_getenv("LOCALAPPDATA", local, sizeof local) || !*local) return 0;
    snprintf(cur, sizeof cur, "%s" SEP "Programs" SEP "OOT-DEBUG-MENU" SEP "app" SEP "current.txt", local);
    uint64_t n = 0;
    char *v = (char *)plat_file_read(cur, &n);
    if (!v) return 0;
    v[strcspn(v, "\r\n \t")] = 0;
    snprintf(out, cap, "%s" SEP "Programs" SEP "OOT-DEBUG-MENU" SEP "app" SEP "%s" SEP "tools" SEP "randomizer.py",
             local, v);
    free(v);
    return plat_file_exists(out);
}

int rando_generate(void) {
    if (state == RANDO_RUNNING) return 0;
    char dir[PLAT_PATH_MAX], python[PLAT_PATH_MAX + 64], tool[PLAT_PATH_MAX + 128], mods[PLAT_PATH_MAX];
    if (!exe_dir(dir, sizeof dir)) {
        snprintf(note, sizeof note, "Cannot find the game's folder.");
        state = RANDO_FAILED;
        return 0;
    }
    if (!find_python(dir, python, sizeof python)) {
        snprintf(note, sizeof note, "Cannot find Python: install the game with the launcher, or set rando_python in "
                                    "nx2navi.ini to a python.exe that has the zstandard package.");
        state = RANDO_FAILED;
        return 0;
    }
    if (!find_tool(dir, tool, sizeof tool)) {
        snprintf(note, sizeof note, "Cannot find tools\\randomizer.py next to the game.");
        state = RANDO_FAILED;
        return 0;
    }
    mods_scan();
    snprintf(mods, sizeof mods, "%s", mods_dir() && *mods_dir() ? mods_dir() : "");
    if (!*mods) snprintf(mods, sizeof mods, "%smods", dir);
    plat_file_mkdir_p(mods);
    char cache[PLAT_PATH_MAX + 32];
    snprintf(cache, sizeof cache, "%sbuild-cache", dir);
    plat_file_mkdir_p(cache);
    snprintf(status_path, sizeof status_path, "%s" SEP "randomizer.status", cache);
    if (!plat_file_write(status_path, "running\n", 8)) {
        snprintf(note, sizeof note, "Cannot write %s.", status_path);
        state = RANDO_FAILED;
        return 0;
    }
    if (!*rando_seed()) rando_new_seed();
    char game[PLAT_PATH_MAX], seed[64];
    snprintf(game, sizeof game, "%s", dir);
    size_t gl = strlen(game);
    if (gl > 3 && (game[gl - 1] == '\\' || game[gl - 1] == '/')) game[gl - 1] = 0;   // "C:\x\" ends the argument's quote
    snprintf(seed, sizeof seed, "%s", rando_seed());
    const char *argv[40];
    int argc = 0;
    argv[argc++] = python;
    argv[argc++] = tool;
    argv[argc++] = "--game";
    argv[argc++] = game;
    argv[argc++] = "--mods";
    argv[argc++] = mods;
    argv[argc++] = "--seed";
    argv[argc++] = seed;
    for (int i = 0; i < rando_noptions; i++) {
        argv[argc++] = rando_options[i].arg;
        argv[argc++] = rando_options[i].values[rando_option_index(i)];
    }
    const char *sp = cfg_get("NX2NAVI_RANDO_SPOILER");
    if (sp && !strcmp(sp, "0")) argv[argc++] = "--no-spoiler";
    argv[argc++] = "--pending";
    argv[argc++] = "--status";
    argv[argc++] = status_path;
    argv[argc] = NULL;
    if (!plat_spawn(argv, game)) {
        snprintf(note, sizeof note, "Cannot start %s.", python);
        state = RANDO_FAILED;
        return 0;
    }
    rt_log("randomizer: generating seed %s with %s", seed, tool);
    state = RANDO_RUNNING;
    started_ms = polled_ms = plat_now_ms();
    snprintf(note, sizeof note, "Generating seed %s...", seed);
    return 1;
}

// The Randomizer mod at the top of the Mods order and switched on; it applies at the next start with the new files.
static void put_first(void) {
    for (int i = 0; i < mods_count(); i++) {
        ModInfo *m = mods_at(i);
        if (strcmp(m->name, RANDO_MOD)) continue;
        while (i > 0) mods_move(i, -1), i--;
        if (!m->enabled) mods_set_enabled(0, 1);
        return;
    }
    // not there at the scan (the first seed): first in the order, out of the switched-off list
    const char *order = cfg_get("NX2NAVI_MODS_ORDER");
    const char *off = cfg_get("NX2NAVI_MODS_DISABLED");
    size_t n = (order ? strlen(order) : 0) + sizeof RANDO_MOD + 2;
    char *o = calloc(n, 1), *d = calloc((off ? strlen(off) : 0) + 1, 1);
    if (!o || !d) {
        free(o), free(d);
        return;
    }
    strcpy(o, RANDO_MOD);
    char *copy = strdup(order ? order : "");
    for (char *s = copy ? strtok(copy, "|") : NULL; s; s = strtok(NULL, "|"))
        if (strcmp(s, RANDO_MOD)) strcat(o, "|"), strcat(o, s);
    free(copy);
    copy = strdup(off ? off : "");
    for (char *s = copy ? strtok(copy, "|") : NULL; s; s = strtok(NULL, "|"))
        if (strcmp(s, RANDO_MOD)) {
            if (*d) strcat(d, "|");
            strcat(d, s);
        }
    free(copy);
    cfg_set("NX2NAVI_MODS_ORDER", o);
    cfg_set("NX2NAVI_MODS_DISABLED", d);
    free(o), free(d);
}

void rando_tick(void) {
    if (state != RANDO_RUNNING) return;
    uint64_t now = plat_now_ms();
    if (now - polled_ms < 500) return;
    polled_ms = now;
    uint64_t n = 0;
    char *s = (char *)plat_file_read(status_path, &n);
    if (s && !strncmp(s, "ok: ", 4)) {
        s[strcspn(s, "\r\n")] = 0;
        put_first();
        cfg_save();
        snprintf(note, sizeof note, "Done: %s. Restart the game to play it.", s + 4);
        rt_log("randomizer: %s", s + 4);
        state = RANDO_DONE;
    } else if (s && !strncmp(s, "error: ", 7)) {
        s[strcspn(s, "\r\n")] = 0;
        snprintf(note, sizeof note, "Failed: %.480s", s + 7);
        rt_log("randomizer: failed: %s", s + 7);
        state = RANDO_FAILED;
    } else if (now - started_ms > RUN_TIMEOUT_MS) {
        snprintf(note, sizeof note, "The generator stopped without a result (no word in %s).", status_path);
        state = RANDO_FAILED;
    } else {
        snprintf(note, sizeof note, "Generating seed %s... %llu s", rando_seed(),
                 (unsigned long long)((now - started_ms) / 1000));
    }
    free(s);
}
