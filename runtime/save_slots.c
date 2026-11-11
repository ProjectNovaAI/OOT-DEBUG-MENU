// Save profiles and manual saves: the layout and the rules are in save_slots.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "save_share.h"
#include "save_slots.h"
#include "save_zip.h"

// Internal helpers run with profile_lock held by their public entry point.
static int save_profile_active_impl(char out[SAVE_NAME_MAX]);
static int save_profile_list_impl(char names[][SAVE_NAME_MAX], int max);
static int save_profile_create_impl(char out[SAVE_NAME_MAX]);
static int save_profile_switch_impl(const char *name);
static int save_profile_switch_now_impl(const char *name);
static int save_profile_adopt_impl(const char *name);
static int save_profile_has_save_impl(const char *name);
static int save_profile_delete_impl(const char *name);
static int save_manual_list_impl(char names[][SAVE_NAME_MAX], int max);
static int save_manual_make_impl(char out[SAVE_NAME_MAX]);
static int save_manual_load_impl(const char *name);
static int save_manual_delete_impl(const char *name);

#define DEFAULT_PROFILE "Default"
#define TAG_PREFIX "profile:"

static int root_dir(char *out, size_t cap) {
    char saves[PLAT_PATH_MAX];
    if (!save_share_saves_dir(saves, sizeof saves)) return 0;
    int n = snprintf(out, cap, "%s/profiles", saves);
    if (n <= 0 || (size_t)n >= cap) return 0;
    plat_file_mkdir_p(out);
    return 1;
}

// A name that is one plain path part: no separators, no dots in front, printable.
static int safe_name(const char *s) {
    if (!*s || *s == '.' || strlen(s) >= SAVE_NAME_MAX) return 0;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' ||
            *p == '|' || (unsigned char)*p < 32)
            return 0;
    return 1;
}

static int path_of(char *out, size_t cap, const char *profile, const char *rest) {
    char root[PLAT_PATH_MAX];
    if (!root_dir(root, sizeof root) || !safe_name(profile)) return 0;
    int n = rest ? snprintf(out, cap, "%s/%s/%s", root, profile, rest) : snprintf(out, cap, "%s/%s", root, profile);
    return n > 0 && (size_t)n < cap;
}

static int write_active(const char *name) {
    char root[PLAT_PATH_MAX], file[PLAT_PATH_MAX + 16];
    if (!root_dir(root, sizeof root)) return -1;
    snprintf(file, sizeof file, "%s/active.txt", root);
    if (!plat_file_write_atomic(file, name, strlen(name))) {
        rt_log("save profiles: cannot write %s", file);
        return -1;
    }
    return 0;
}

static int save_profile_active_impl(char out[SAVE_NAME_MAX]) {
    snprintf(out, SAVE_NAME_MAX, "%s", DEFAULT_PROFILE);
    char root[PLAT_PATH_MAX], file[PLAT_PATH_MAX + 16], tag[160];
    if (!root_dir(root, sizeof root)) return 0;
    // a switch the last start made: its profile is the one in use now
    if (fs_save_import_tag_peek(tag, sizeof tag) && !strncmp(tag, TAG_PREFIX, strlen(TAG_PREFIX)) &&
        safe_name(tag + strlen(TAG_PREFIX))) {
        // Leave the identity and its archive intact if active.txt cannot be replaced. Every caller must
        // stop rather than treat the imported save as progress of the previous profile.
        if (write_active(tag + strlen(TAG_PREFIX))) return 0;
        if (fs_save_import_tag_ack(tag)) {
            rt_log("save profiles: cannot acknowledge the imported profile identity; it will retry");
            return 0;
        }
        rt_log("save profiles: the profile in use is %s now", tag + strlen(TAG_PREFIX));
        // its progress.zip is the game's save now (and the start backed up the save it replaced): a copy left there
        // would go stale as the game plays on
        char kept[PLAT_PATH_MAX];
        if (path_of(kept, sizeof kept, tag + strlen(TAG_PREFIX), "progress.zip")) plat_file_delete(kept);
    }
    snprintf(file, sizeof file, "%s/active.txt", root);
    uint64_t n = 0;
    uint8_t *d = plat_file_read(file, &n);
    if (d) {
        char name[SAVE_NAME_MAX];
        size_t k = n < sizeof name ? (size_t)n : sizeof name - 1;
        memcpy(name, d, k);
        name[k] = 0;
        free(d);
        while (k && (name[k - 1] == '\n' || name[k - 1] == '\r' || name[k - 1] == ' ')) name[--k] = 0;
        if (safe_name(name)) snprintf(out, SAVE_NAME_MAX, "%s", name);
    }
    char dir[PLAT_PATH_MAX];
    if (path_of(dir, sizeof dir, out, NULL)) plat_file_mkdir_p(dir);
    return 1;
}

typedef struct {
    char (*names)[SAVE_NAME_MAX];
    int n, max, dirs;   // dirs: list folders, else .zip files (the name without .zip)
} Collect;

static int collect(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Collect *c = ctx;
    if (c->n >= c->max) return 0;
    char t[SAVE_NAME_MAX];
    if (c->dirs) {
        if (kind != PLAT_FS_DIR || !safe_name(name)) return 1;
        snprintf(t, sizeof t, "%s", name);
    } else {
        size_t len = strlen(name);
        if (kind != PLAT_FS_FILE || len < 5 || len - 4 >= sizeof t || strcmp(name + len - 4, ".zip")) return 1;
        memcpy(t, name, len - 4);
        t[len - 4] = 0;
    }
    snprintf(c->names[c->n++], SAVE_NAME_MAX, "%s", t);
    return 1;
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int by_name_desc(const void *a, const void *b) { return -strcmp((const char *)a, (const char *)b); }

static int save_profile_list_impl(char names[][SAVE_NAME_MAX], int max) {
    char root[PLAT_PATH_MAX], active[SAVE_NAME_MAX];
    if (!save_profile_active_impl(active) || !root_dir(root, sizeof root)) return 0;
    Collect c = {names, 0, max, 1};
    plat_fs_list(root, collect, &c);
    qsort(names, (size_t)c.n, SAVE_NAME_MAX, by_name);
    return c.n;
}

static int save_profile_create_impl(char out[SAVE_NAME_MAX]) {
    char dir[PLAT_PATH_MAX];
    for (int k = 2; k < 1000; k++) {
        snprintf(out, SAVE_NAME_MAX, "Profile %d", k);
        if (!path_of(dir, sizeof dir, out, NULL)) return -1;
        if (plat_fs_kind(dir) != PLAT_FS_MISSING) continue;
        plat_file_mkdir_p(dir);
        rt_log("save profiles: made %s", out);
        return plat_dir_exists(dir) ? 0 : -1;
    }
    return -1;
}

// Puts the game's save into the profile in use and stages the save of name, tagged with its profile, as the import.
// *empty: the profile in use or name has no save (one of them starts a new game). 0 on success.
static int stage_switch(const char *name, int *empty) {
    char active[SAVE_NAME_MAX], mine[PLAT_PATH_MAX], theirs[PLAT_PATH_MAX], dir[PLAT_PATH_MAX];
    if (!save_profile_active_impl(active) || !safe_name(name) || !strcmp(name, active)) return -1;
    if (!path_of(dir, sizeof dir, name, NULL) || !plat_dir_exists(dir)) return -1;
    if (!path_of(mine, sizeof mine, active, "progress.zip") || !path_of(theirs, sizeof theirs, name, "progress.zip"))
        return -1;
    // the profile in use keeps the game's save; with none yet its progress is a new game
    int active_has_save = save_profile_has_save_impl(active);
    int target_has_save = save_profile_has_save_impl(name);
    SaveShareResult r = save_share_export_to(mine);
    if (r == SAVE_SHARE_NO_SAVE) plat_file_delete(mine);
    else if (r != SAVE_SHARE_OK) return -1;
    char detail[160];
    int none = !plat_file_exists(theirs);
    *empty = !active_has_save || !target_has_save;
    int staged = !none ? save_share_import(theirs, detail, sizeof detail) == SAVE_SHARE_OK
                         : save_share_stage_empty() == 0;
    char tag[SAVE_NAME_MAX + 16];
    snprintf(tag, sizeof tag, TAG_PREFIX "%s", name);
    if (!staged || fs_save_import_tag(tag)) {
        if (staged) save_share_cancel();
        rt_log("save profiles: cannot stage the save of %s", name);
        return -1;
    }
    return 0;
}

static int save_profile_switch_impl(const char *name) {
    char active[SAVE_NAME_MAX];
    int empty;
    if (!save_profile_active_impl(active) || stage_switch(name, &empty)) return -1;
    rt_log("save profiles: %s keeps the save; %s's save waits for the next start", active, name);
    return 0;
}

static int save_profile_switch_now_impl(const char *name) {
    char active[SAVE_NAME_MAX];
    int empty;
    if (!save_profile_active_impl(active) || stage_switch(name, &empty)) return -1;
    if (empty) {
        // the title shows Continue only when there is a save, and builds that menu at the start
        rt_log("save profiles: %s or %s has no save; %s's save waits for the next start", active, name, name);
        return 1;
    }
    int r = fs_save_import_now();
    if (r < 0) {
        save_share_cancel();
        rt_log("save profiles: cannot switch to %s while the game runs; %s stays in use", name, active);
        return -1;
    }
    if (!save_profile_active_impl(active)) {
        rt_log("save profiles: imported %s but cannot record its identity; restart required", name);
        return 1;
    }   // acknowledges the import's tag only after active.txt names the new profile
    rt_log("save profiles: switched to %s while the game runs%s", name, r ? "; the game restarts to finish" : "");
    return r;
}

// Save As: keep the running session, but give its next save to another profile. The old profile keeps its
// last committed save. The destination's previous progress is preserved as a manual save before it becomes active.
static int save_profile_adopt_impl(const char *name) {
    char active[SAVE_NAME_MAX], mine[PLAT_PATH_MAX], target[PLAT_PATH_MAX], dir[PLAT_PATH_MAX];
    if (!save_profile_active_impl(active) || !safe_name(name) || fs_save_busy() || save_share_pending()) return -1;
    if (!strcmp(active, name)) return 0;
    if (!path_of(dir, sizeof dir, name, NULL) || !plat_dir_exists(dir) ||
        !path_of(mine, sizeof mine, active, "progress.zip") ||
        !path_of(target, sizeof target, name, "progress.zip")) return -1;
    SaveShareResult r = save_share_export_to(mine);
    if (r == SAVE_SHARE_NO_SAVE) plat_file_delete(mine);
    else if (r != SAVE_SHARE_OK) return -1;
    if (plat_file_exists(target)) {
        char manual[PLAT_PATH_MAX], backup[PLAT_PATH_MAX];
        if (!path_of(manual, sizeof manual, name, "manual")) return -1;
        plat_file_mkdir_p(manual);
        struct tm t;
        plat_local_time(&t);
        int saved = 0;
        for (int k = 1; k < 1000; k++) {
            int n = snprintf(backup, sizeof backup, "%s/%04d-%02d-%02d %02d.%02d.%02d Before save (%d).zip",
                             manual, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, k);
            if (n < 0 || (size_t)n >= sizeof backup) return -1;
            if (plat_file_exists(backup)) continue;
            uint64_t size = 0;
            uint8_t *data = plat_file_read(target, &size);
            if (!data) return -1;
            saved = plat_file_write_atomic(backup, data, (size_t)size);
            free(data);
            break;
        }
        if (!saved) return -1;
    }
    if (write_active(name)) return -1;
    plat_file_delete(target);   // the running save is authoritative for the active profile
    rt_log("save profiles: current session will save into %s; %s keeps its previous save", name, active);
    return 0;
}

static int count_slot(void *ctx, const char *rel, const uint8_t *data, size_t n) {
    (void)data, (void)n;
    if (strlen(rel) == 20 && !strncmp(rel, "slot_", 5) && rel[5] >= '0' && rel[5] <= '9' &&
        rel[6] >= '0' && rel[6] <= '9' && !strcmp(rel + 7, "/progress.sav")) ++*(int *)ctx;
    return 1;
}

static int save_profile_has_save_impl(const char *name) {
    char active[SAVE_NAME_MAX], zip[PLAT_PATH_MAX];
    if (!save_profile_active_impl(active)) return 0;
    if (strcmp(name, active)) {
        if (!path_of(zip, sizeof zip, name, "progress.zip")) return 0;
        uint64_t n = 0;
        uint8_t *data = plat_file_read(zip, &n);
        if (!data) return 0;
        SzEntry *entries = NULL;
        int count = 0, slots = 0;
        if (sz_read_dir(data, (size_t)n, &entries, &count) == SZ_OK)
            for (int i = 0; i < count; i++)
                if (!entries[i].is_dir && !strncmp(entries[i].name, "save/", 5))
                    count_slot(&slots, entries[i].name + 5, NULL, 0);
        free(entries);
        free(data);
        return slots > 0;
    }
    int slots = 0;
    return fs_save_export(count_slot, &slots) > 0 && slots > 0;
}

// A profile folder: its files, its manual folder's files, the folders.
static int remove_files(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    char p[PLAT_PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", (const char *)ctx, name);
    if (kind == PLAT_FS_FILE) plat_fs_unlink(p);
    return 1;
}

static int save_profile_delete_impl(const char *name) {
    char active[SAVE_NAME_MAX], dir[PLAT_PATH_MAX], manual[PLAT_PATH_MAX];
    if (!save_profile_active_impl(active) || !safe_name(name) || !strcmp(name, active)) return -1;
    if (!path_of(dir, sizeof dir, name, NULL) || !path_of(manual, sizeof manual, name, "manual")) return -1;
    if (plat_dir_exists(manual)) {
        plat_fs_list(manual, remove_files, manual);
        plat_fs_rmdir(manual);
    }
    plat_fs_list(dir, remove_files, dir);
    int bad = plat_fs_rmdir(dir) != 0 && plat_fs_kind(dir) != PLAT_FS_MISSING;
    rt_log("save profiles: %s %s", bad ? "cannot delete all of" : "deleted", name);
    return bad ? -1 : 0;
}

static int save_manual_list_impl(char names[][SAVE_NAME_MAX], int max) {
    char active[SAVE_NAME_MAX], manual[PLAT_PATH_MAX];
    if (!save_profile_active_impl(active) || !path_of(manual, sizeof manual, active, "manual")) return 0;
    Collect c = {names, 0, max, 0};
    plat_fs_list(manual, collect, &c);
    qsort(names, (size_t)c.n, SAVE_NAME_MAX, by_name_desc);
    return c.n;
}

static int save_manual_make_impl(char out[SAVE_NAME_MAX]) {
    char active[SAVE_NAME_MAX], manual[PLAT_PATH_MAX], file[PLAT_PATH_MAX + SAVE_NAME_MAX];
    if (!save_profile_active_impl(active) || !path_of(manual, sizeof manual, active, "manual")) return -1;
    plat_file_mkdir_p(manual);
    struct tm t;
    plat_local_time(&t);
    char area[64] = "";
    if (dbg_available() && dbg_current_area(area))
        for (char *p = area; *p; p++)
            if (!(*p >= 'A' && *p <= 'Z') && !(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_') *p = '_';
    for (int k = 1; k < 100; k++) {
        char stem[SAVE_NAME_MAX];
        snprintf(stem, sizeof stem, "%04d-%02d-%02d %02d.%02d.%02d%s%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec, *area ? " " : "", area);
        if (k > 1) {
            size_t len = strlen(stem);
            snprintf(stem + len, sizeof stem - len, " (%d)", k);
        }
        snprintf(file, sizeof file, "%s/%s.zip", manual, stem);
        if (plat_fs_kind(file) != PLAT_FS_MISSING) continue;
        SaveShareResult r = save_share_export_to(file);
        if (r == SAVE_SHARE_NO_SAVE) return 1;
        if (r != SAVE_SHARE_OK) return -1;
        snprintf(out, SAVE_NAME_MAX, "%s", stem);
        return 0;
    }
    return -1;
}

static int manual_file(char *out, size_t cap, const char *name) {
    char active[SAVE_NAME_MAX], rest[SAVE_NAME_MAX + 16];
    if (!save_profile_active_impl(active) || !safe_name(name)) return 0;
    snprintf(rest, sizeof rest, "manual/%s.zip", name);
    return path_of(out, cap, active, rest) && plat_file_exists(out);
}

static int save_manual_load_impl(const char *name) {
    char file[PLAT_PATH_MAX], detail[160];
    if (!manual_file(file, sizeof file, name)) return -1;
    return save_share_import(file, detail, sizeof detail) == SAVE_SHARE_OK ? 0 : -1;
}

static int save_manual_delete_impl(const char *name) {
    char file[PLAT_PATH_MAX];
    if (!manual_file(file, sizeof file, name)) return -1;
    plat_file_delete(file);
    rt_log("save profiles: deleted the manual save %s", name);
    return plat_file_exists(file) ? -1 : 0;
}

// The title picker, debug menu and console each run file work on a different thread. One operation owns
// the profile archives and active marker at a time; internal helpers share that lock.
static PlatMutex profile_lock = PLAT_MUTEX_INIT;
int save_profile_active(char out[SAVE_NAME_MAX]) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_active_impl(out);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_list(char names[][SAVE_NAME_MAX], int max) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_list_impl(names, max);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_create(char out[SAVE_NAME_MAX]) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_create_impl(out);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_switch(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_switch_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_switch_now(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_switch_now_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_adopt(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_adopt_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_has_save(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_has_save_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_profile_delete(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_profile_delete_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_manual_list(char names[][SAVE_NAME_MAX], int max) {
    plat_mutex_lock(&profile_lock);
    int result = save_manual_list_impl(names, max);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_manual_make(char out[SAVE_NAME_MAX]) {
    plat_mutex_lock(&profile_lock);
    int result = save_manual_make_impl(out);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_manual_load(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_manual_load_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
int save_manual_delete(const char *name) {
    plat_mutex_lock(&profile_lock);
    int result = save_manual_delete_impl(name);
    plat_mutex_unlock(&profile_lock);
    return result;
}
