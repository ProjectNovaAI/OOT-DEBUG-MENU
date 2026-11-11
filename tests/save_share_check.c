// runtime/fs_host.c, save_share.c and save_zip.c without the game, driven by tests/save_share_check.py. Each run of
// this program is one start of the game on the profile folder it is given: the first storage use runs storage_init,
// which makes a waiting import the save (fs_host.c import_pending). The rest of the runtime is stubbed below: only what
// the save paths call does anything.
//   save_share_check export PROFILE DIR [bug]   save_share_export into DIR; prints "result N path P"
//   save_share_check import PROFILE ZIP         save_share_import; prints "result N detail D waiting W"
//   save_share_check start PROFILE              a start and nothing else; prints "done D waiting W"
//   save_share_check cancel PROFILE             save_share_cancel; prints "result N waiting W"
//   save_share_check adopt PROFILE NAME STATE [ZIP]   save current session as another profile
//   save_share_check has-save PROFILE NAME            playable-slot presence
//   save_share_check live PROFILE ZIP STATE   live import; STATE clean|unmounted|writer|dirty
// Backups go to saves/ beside this program (save_share_backup_dir), as the game's go beside the game.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save_share.h"
#include "cpu.h"
#include "save_slots.h"
#include "platform/platform.h"

// fs.h's own declarations of these would clash with the stubs below, which only have to link
void fs_host_set_profile(const char *dir);
const char *fs_save_import_done(void);
int fs_save_export(int (*visit)(void *ctx, const char *rel, const uint8_t *data, size_t n), void *ctx);

static int count_file(void *ctx, const char *rel, const uint8_t *data, size_t n) {
    (void)ctx, (void)rel, (void)data, (void)n;
    return 1;
}

// ---- stubs for the runtime around fs_host.c (host.h and the files that define these in the game)
const int rt_game_version = 111;
const char rt_port_version[] = "9.9.9-check";

static volatile int32_t race_enabled, race_started, race_finished;
static PlatThread *race_thread;
static void racing_writer(void *arg);

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (plat_cas32(&race_enabled, 0, 0) && strstr(fmt, "the save imported in the menu is the save now")) {
        plat_xchg32(&race_enabled, 0);
        race_thread = plat_thread_new(racing_writer, NULL, "racing save writer");
        if (!race_thread) exit(80);
        while (!plat_cas32(&race_started, 0, 0)) plat_sleep_ms(1);
        plat_sleep_ms(50);
        // The import has passed its safety check, but has not swapped its working copy yet.
        // A new writable handle must wait for that swap, never attach to the discarded copy.
        if (plat_cas32(&race_finished, 0, 0)) exit(81);
    }
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(99);
}

const char *rt_log_path(void) { return getenv("SAVE_SHARE_CHECK_LOG"); }
void *rt_config(void) { return NULL; }   // fs_host_set_profile names the profile first
void account_uid(uint64_t uid[2]) { uid[0] = 0x1111222233334444ull, uid[1] = 0x5555666677778888ull; }
int dbg_available(void) { return 0; }
int dbg_current_area(char *out) { (void)out; return 0; }
int folder_downloads(char *out, size_t cap) { (void)out, (void)cap; return 0; }

// never reached on the save paths: RomFS, the host function registry, guest memory, the probes
#define NEVER(name) \
    void name(void) { rt_fatal(#name " called"); }
NEVER(fs_init) NEVER(hostfn_register) NEVER(mem_kernel_alloc) NEVER(mem_kernel_free)
NEVER(mem_thaw) NEVER(romfs_close) NEVER(romfs_entry) NEVER(romfs_entry_type) NEVER(romfs_file_count)
NEVER(romfs_image_path) NEVER(romfs_list) NEVER(romfs_open) NEVER(romfs_overlay)
NEVER(romfs_overlay_count) NEVER(romfs_read) NEVER(romfs_size) NEVER(rt_call)
NEVER(perf_size)   // fs_host_sizes: only the perf trace's size lines call it
int romfs_handle(uint64_t h) { (void)h; return 0; }
int romfs_owns(const char *path) { (void)path; return 0; }
int mem_probing(void) { return 0; }   // write_file asks before each write: no probe, no lazy heap here
void mem_kernel_access(uint64_t a, uint64_t n) { (void)a, (void)n; }
void fs_trace(const char *what, const char *path) { (void)what, (void)path; }
void coop_game_rom_open(const char *path) { (void)path; }   // co-op watches for the title screen's start: not here

// Resolve the same file-service thunks the game imports; exercise the mounted working copy through its public ABI.
typedef struct { const char *name; GuestFn fn; } TestHostEntry;
#ifdef _WIN32
__attribute__((section(".ovr$a"), used)) static const TestHostEntry test_start[1];
__attribute__((section(".ovr$z"), used)) static const TestHostEntry test_stop[1];
#else
extern const TestHostEntry __start_nx2navi_ovr[], __stop_nx2navi_ovr[];
#define test_start __start_nx2navi_ovr
#define test_stop __stop_nx2navi_ovr
#endif
GuestFn hostfn_override(const char *name) {
    const TestHostEntry *p = test_start, *end = test_stop;
    __asm__("" : "+r"(p), "+r"(end));
    for (; p < end; p++) if (p->name && !strcmp(p->name, name)) return p->fn;
    rt_fatal("missing service %s", name);
    return NULL;
}
static uint64_t service(const char *name, uint64_t a, uint64_t b, uint64_t d) {
    Cpu c = {0};
    c.x[0] = a, c.x[1] = b, c.x[2] = d;
    hostfn_override(name)(&c);
    return c.x[0];
}
#define PTR(p) ((uint64_t)(uintptr_t)(p))
static void racing_writer(void *arg) {
    (void)arg;
    uint64_t h = 0;
    plat_xchg32(&race_started, 1);
    if (service("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", PTR(&h), PTR("save:/option.sav"), 2)) exit(82);
    if (service("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", h, 3, 0)) exit(83);
    service("_ZN2nn2fs9CloseFileENS0_10FileHandleE", h, 0, 0);
    plat_xchg32(&race_finished, 1);
}
int fs_save_import_now(void);
int fs_save_import_tag(const char *tag);
int fs_save_import_tag_take(char *out, size_t cap);

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    fs_host_set_profile(argv[2]);
    const char *mode = argv[1];
    if (!strcmp(mode, "export") && argc >= 4) {
        char path[2048];
        SaveShareResult r = save_share_export(argv[3], argc > 4 && !strcmp(argv[4], "bug"), path, sizeof path);
        printf("result %d path %s\n", (int)r, path);
    } else if (!strcmp(mode, "import") && argc == 4) {
        char detail[256];
        SaveShareResult r = save_share_import(argv[3], detail, sizeof detail);
        printf("result %d detail %s waiting %d\n", (int)r, detail, save_share_pending());
    } else if (!strcmp(mode, "active")) {
        char active[SAVE_NAME_MAX];
        int r = save_profile_active(active);
        printf("result %d active %s\n", r, active);
    } else if (!strcmp(mode, "has-save") && argc == 4) {
        printf("has %d\n", save_profile_has_save(argv[3]));
    } else if (!strcmp(mode, "adopt") && argc >= 5) {
        fs_save_export(count_file, NULL);
        uint64_t uid[2], handle = 0;
        account_uid(uid);
        if (service("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", PTR("save"), PTR(uid), 0)) return 3;
        if (!strcmp(argv[4], "writer") && service("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", PTR(&handle), PTR("save:/option.sav"), 2)) return 4;
        if (!strcmp(argv[4], "dirty") && service("_ZN2nn2fs10CreateFileEPKcl", PTR("save:/uncommitted.sav"), 0, 0)) return 5;
        char detail[256], active[SAVE_NAME_MAX];
        if (!strcmp(argv[4], "pending") && (argc < 6 || save_share_import(argv[5], detail, sizeof detail) != SAVE_SHARE_OK)) return 6;
        int r = save_profile_adopt(argv[3]);
        save_profile_active(active);
        printf("result %d active %s\n", r, active);
        if (handle) service("_ZN2nn2fs9CloseFileENS0_10FileHandleE", handle, 0, 0);
    } else if (!strcmp(mode, "live") && argc == 5) {
        const char *state = argv[4];
        fs_save_export(count_file, NULL);   // boot first, before staging a live import
        uint64_t uid[2];
        account_uid(uid);
        if (strcmp(state, "unmounted") && service("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", PTR("save"), PTR(uid), 0))
            return 3;
        uint64_t handle = 0;
        if (!strcmp(state, "writer") && service("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", PTR(&handle), PTR("save:/option.sav"), 2))
            return 4;
        if (!strcmp(state, "dirty") && service("_ZN2nn2fs10CreateFileEPKcl", PTR("save:/uncommitted.sav"), 0, 0))
            return 5;
        char detail[256], tag[64] = "";
        if (save_share_import(argv[3], detail, sizeof detail) != SAVE_SHARE_OK || fs_save_import_tag("profile:Live")) return 6;
        if (!strcmp(state, "race")) plat_xchg32(&race_enabled, 1);
        int r = fs_save_import_now();
        if (race_thread) plat_thread_join(race_thread);
        int tagged = fs_save_import_tag_take(tag, sizeof tag);
        printf("result %d waiting %d tagged %d tag %s\n", r, save_share_pending(), tagged, tag);
        if (handle) service("_ZN2nn2fs9CloseFileENS0_10FileHandleE", handle, 0, 0);
    } else if (!strcmp(mode, "start")) {
        fs_save_export(count_file, NULL);   // the first storage use: storage_init and its import_pending
        printf("done %s waiting %d\n", fs_save_import_done(), save_share_pending());
    } else if (!strcmp(mode, "cancel")) {
        int r = save_share_cancel();
        printf("result %d waiting %d\n", r, save_share_pending());
    } else {
        return 2;
    }
    return 0;
}

