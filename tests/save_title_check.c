// Profile-picker input regression checks with queued workers and a simulated game UI, no game files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_slots.h"
#include "input_map.h"
#include "platform/platform.h"
static uint64_t now = 1000000000ull;
static uint32_t ui = 0x42814d01u;
static int active, present[3] = {1, 1, 1}, switch_result, adopt_result;
static int switches, adopts, reloads, interleave, enabled = 1;
static PlatThreadFn worker;
static void *worker_arg;
static const char *names[] = {"Alpha", "Beta", "Gamma"};
#define A (1ull << B_A)
#define B (1ull << B_B)
#define L (1ull << B_L)
#define R (1ull << B_R)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint64_t input(uint64_t b) { now += 110000000ull; save_title_input(&b); return b; }
static uint64_t tap(uint64_t b) { input(0); return input(b); }
int cfg_bool(const char *name) { (void)name; return enabled; }
int dbg_available(void) { return 1; }
int dbg_flag_get_u32(int type, uint32_t key, uint32_t *out) { (void)type; (void)key; *out = ui; return 1; }
uint64_t plat_now_ns(void) { return now; }
void plat_sleep_ms(uint32_t ms) { now += (uint64_t)ms * 1000000; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name; (void)stack; CHECK(!worker); worker = fn; worker_arg = arg; return 1;
}
static void finish(void) { CHECK(worker); PlatThreadFn fn = worker; worker = NULL; fn(worker_arg); }
int fs_save_busy(void) { return 0; }
int save_profile_list(char out[][SAVE_NAME_MAX], int max) {
    CHECK(max >= 3); for (int i = 0; i < 3; i++) snprintf(out[i], SAVE_NAME_MAX, "%s", names[i]); return 3;
}
int save_profile_active(char out[SAVE_NAME_MAX]) { snprintf(out, SAVE_NAME_MAX, "%s", names[active]); return 1; }
int save_profile_has_save(const char *name) { for (int i = 0; i < 3; i++) if (!strcmp(name, names[i])) return present[i]; return 0; }
static int index_of(const char *name) { for (int i = 0; i < 3; i++) if (!strcmp(name, names[i])) return i; abort(); }
int save_profile_switch_now(const char *name) {
    int target = index_of(name); switches++;
    if (interleave) { interleave = 0; tap(R); } // a new selection arrives while file work runs unlocked
    if (!switch_result) active = target;
    return switch_result;
}
int save_profile_adopt(const char *name) { adopts++; if (!adopt_result) active = index_of(name); return adopt_result; }
int dbg_save_list_reload(void) { reloads++; return 1; }
int dbg_save_list_state(void) { return 1; }
void rt_log(const char *fmt, ...) { (void)fmt; }
static SaveTitleView view(void) { SaveTitleView v; CHECK(save_title_view(&v)); return v; }
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "save")) {
        input(0); CHECK(view().saving); CHECK(!(tap(R) & R));
        CHECK(view().phase == SAVE_TITLE_CHOSEN && !switches && !adopts && !worker);
        CHECK(!(tap(A) & A)); CHECK(worker && !switches && !adopts);
        CHECK(!(input(A | B) & (A | B)));
        CHECK(!(tap(R) & R)); CHECK(!strcmp(view().name, "Beta")); // destination cannot move once preparation starts
        finish();
        CHECK(active == 1 && adopts == 1 && !switches && !reloads);
        CHECK(input(0) & A); CHECK(!(input(0) & A)); // resume exactly one save confirmation
    } else if (!strcmp(argv[1], "cancel")) {
        input(0); tap(R); CHECK(!(tap(B) & B));
        CHECK(view().phase == SAVE_TITLE_IDLE && !strcmp(view().name, "Alpha"));
        CHECK(!worker && !switches && !adopts); CHECK(tap(A) & A);
    } else if (!strcmp(argv[1], "empty")) {
        ui = 0x43080996u; present[1] = 0; switch_result = 1;
        input(0); tap(R); CHECK(view().phase == SAVE_TITLE_CHOSEN && !worker);
        CHECK(!(tap(A) & A)); finish(); CHECK(switches == 1 && !adopts);
        CHECK(view().phase == SAVE_TITLE_RESTART); CHECK(save_title_restart_due()); CHECK(!save_title_restart_due());
    } else if (!strcmp(argv[1], "queued")) {
        ui = 0x43080996u; input(0); tap(R); CHECK(worker); interleave = 1; finish();
        CHECK(active == 2 && switches == 2 && reloads == 2 && !adopts);
        CHECK(view().phase == SAVE_TITLE_IDLE && !strcmp(view().name, "Gamma"));
    } else if (!strcmp(argv[1], "queued-empty")) {
        ui = 0x43080996u; present[2] = 0; input(0); tap(R); interleave = 1; finish();
        CHECK(active == 1 && switches == 1 && view().phase == SAVE_TITLE_CHOSEN);
        CHECK(!strcmp(view().name, "Gamma") && !worker);
        switch_result = 1; CHECK(!(tap(A) & A)); finish();
        CHECK(switches == 2 && view().phase == SAVE_TITLE_RESTART);
    } else if (!strcmp(argv[1], "failure")) {
        adopt_result = -1; input(0); tap(R); CHECK(!(tap(A) & A)); finish();
        CHECK(active == 0 && adopts == 1 && !switches && view().phase == SAVE_TITLE_FAILED);
        CHECK(!(input(0) & A)); CHECK(!(tap(A) & A)); CHECK(!worker);
        now += 5000000000ull; CHECK(!(tap(A) & A)); // save failures never expire into a save for the wrong profile
        CHECK(!(tap(B) & B)); CHECK(view().phase == SAVE_TITLE_IDLE);
        CHECK(tap(A) & A); // dismissal explicitly returns to the current profile

    } else if (!strcmp(argv[1], "context")) {
        ui = 0; input(0); CHECK(!save_title_showing());
        ui = 0x3b849e33u; input(0); CHECK(!save_title_showing()); // in-game Load is not title Load
        ui = 0x43080996u; input(0); CHECK(save_title_showing());
        ui = 0xa60ba395u; input(0); CHECK(!save_title_showing());
        ui = 0x3b849e33u; input(0); CHECK(save_title_showing()); // title Option -> Load keeps context
        ui = 0; input(0); ui = 0x3b849e33u; input(0); CHECK(!save_title_showing());
    } else return 2;
    printf("%s: ok\n", argv[1]); return 0;
}
