// Hotkeys the player binds to the debug tools (debug_tools.h, "Hotkeys the player binds"): each action has a setting
// debug_hotkey_<name> holding up to two keys and one controller button in the bind line format ("key:g, pad:dpad_up").
// The game's input poll (input.c poll_merged) hands over the keys and physical buttons held; a bound one is cleared before
// the bindings to the game's buttons are applied, so the game never sees it, and its press runs the action once the
// poll has let go of the input lock.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui/menu.h"

typedef struct {
    DbgBindInfo info;
    void (*run)(void);
} Action;

static void say(const char *what, int on) {
    char t[64];
    snprintf(t, sizeof t, "%s %s", what, on ? "on" : "off");
    dbg_toast_show(t);
}

// A setting the renderer or the overlay reads at once, flipped and written to nx2navi.ini a moment later.
static int flip_setting(const char *key) {
    int on = !cfg_bool(key);
    cfg_set(key, on ? "1" : "0");
    menu_save_settings_soon();
    return on;
}

static void readout(int bit, const char *what) {
    int mask = dbg_overlay_mask() ^ bit;
    dbg_overlay_set(mask);
    say(what, (mask & bit) != 0);
}

static void a_fly(void) { dbg_fly(!dbg_fly_on()), say("Fly", dbg_fly_on()); }
static void a_hover(void) { dbg_hover(!dbg_hover_on()), say("Hover", dbg_hover_on()); }
static void a_boots(void) { dbg_hover_boots(!dbg_hover_boots_on()), say("Infinite Hover Boots", dbg_hover_boots_on()); }
static void a_god(void) { dbg_invulnerable(!dbg_invulnerable_on()), say("Invulnerable", dbg_invulnerable_on()); }
static void a_onehit(void) { dbg_one_hit(!dbg_one_hit_on()), say("One-hit kills", dbg_one_hit_on()); }
static void a_magic(void) { dbg_magic_infinite(!dbg_magic_infinite_on()), say("Infinite magic", dbg_magic_infinite_on()); }
static void a_items(void) { dbg_items_infinite(!dbg_items_infinite_on()), say("Infinite items", dbg_items_infinite_on()); }
static void a_anyage(void) { dbg_any_age_items(!dbg_any_age_items_on()), say("Items at any age (WIP)", dbg_any_age_items_on()); }
static void a_fog(void) { say("Fog off", flip_setting("NX2NAVI_DEBUG_FOG_OFF")); }
static void a_walls(void) { say("Invisible walls", flip_setting("NX2NAVI_DEBUG_VIEW_WALLS")); }
static void a_loading(void) { say("Loading zones", flip_setting("NX2NAVI_DEBUG_VIEW_LOADING")); }
static void a_triggers(void) { say("Trigger zones", flip_setting("NX2NAVI_DEBUG_VIEW_TRIGGERS")); }
static void a_hidden(void) { say("Hidden things", flip_setting("NX2NAVI_DEBUG_VIEW_HIDDEN")); }
static void a_death(void) { say("Death barriers", flip_setting("NX2NAVI_DEBUG_VIEW_DEATH")); }
static void a_hook(void) { say("Hookshot targets", flip_setting("NX2NAVI_DEBUG_VIEW_HOOKSHOT")); }
static void a_health(void) { say("Enemy health", flip_setting("NX2NAVI_DEBUG_VIEW_HEALTH")); }
static void a_pos(void) { readout(DBG_SHOW_POS, "Position readout"); }
static void a_speed(void) { readout(DBG_SHOW_SPEED, "Speed readout"); }
static void a_area(void) { readout(DBG_SHOW_AREA, "Area readout"); }
static void a_input(void) { readout(DBG_SHOW_INPUT, "Inputs readout"); }
static void a_reload(void) { dbg_toast_show(dbg_reload_area() ? "Reloading the area" : "This area has no entrance to reload at"); }
static void a_age(void) {
    dbg_toast_show(dbg_age_switch_target() >= 0 && dbg_age_switch() ? "Switching age" : "This area has one age only");
}
static void a_save(void) { dbg_toast_show(dbg_slot_save(0) ? "Position saved to slot 1" : "No player to save"); }
static void a_load(void) { dbg_toast_show(dbg_slot_load(0) ? "Moved to slot 1" : "Slot 1 is empty or no player"); }
static void a_slower(void) {
    char t[48];
    snprintf(t, sizeof t, "Game speed %.3gx", dbg_speed_step(-1));
    dbg_toast_show(t);
}
static void a_faster(void) {
    char t[48];
    snprintf(t, sizeof t, "Game speed %.3gx", dbg_speed_step(1));
    dbg_toast_show(t);
}
static void a_timer(void) { dbg_timer_toggle(), dbg_toast_show(dbg_timer_running() ? "Timer started" : "Timer stopped"); }
static void a_igt(void) {
    dbg_igt_toggle();
    dbg_toast_show(dbg_igt_running() ? "Speedrun timer started" : "Speedrun timer stopped");
}
static void a_split(void) {
    char t[48];
    dbg_igt_split();
    snprintf(t, sizeof t, "Split %d", dbg_igt_splits());
    dbg_toast_show(dbg_igt_running() ? t : "Start the speedrun timer first");
}

// The order of the Debug tab's Hotkeys group. A name is the setting's suffix; renaming one loses the players' binds.
static const Action actions[] = {
    {{"fly", "Fly (no clip)", "Switches Fly on and off: walls and the ground do not stop the player."}, a_fly},
    {{"hover", "Hover", "Switches Hover on and off: the player keeps the height it was switched on at."}, a_hover},
    {{"hoverboots", "Infinite Hover Boots", "Switches Infinite Hover Boots on and off."}, a_boots},
    {{"god", "Invulnerable", "Switches Invulnerable on and off."}, a_god},
    {{"onehit", "One-hit kills", "Switches One-hit kills on and off."}, a_onehit},
    {{"infmagic", "Infinite magic", "Switches Infinite magic on and off."}, a_magic},
    {{"infitems", "Infinite items", "Switches Infinite items on and off."}, a_items},
    {{"anyage", "Items at any age (WIP)", "Switches Items at any age (work in progress) on and off."}, a_anyage},
    {{"reload", "Reload area", "Warps back into this area at the entrance the player last came in by."}, a_reload},
    {{"ageswitch", "Switch age here", "Warps into this area's version for the other age, where it has one."}, a_age},
    {{"savepos", "Save position", "Keeps the player's position in slot 1."}, a_save},
    {{"loadpos", "Go to saved position", "Moves the player to the position kept in slot 1."}, a_load},
    {{"slower", "Game speed down", "One step slower, down to an eighth."}, a_slower},
    {{"faster", "Game speed up", "One step faster, up to four times."}, a_faster},
    {{"fog", "Fog off", "Switches the fog off and back on. Saved in the settings."}, a_fog},
    {{"walls", "Show invisible walls", "Switches the invisible walls overlay. Saved in the settings."}, a_walls},
    {{"loading", "Show loading zones", "Switches the loading zones overlay. Saved in the settings."}, a_loading},
    {{"triggers", "Show trigger zones", "Switches the trigger zones overlay. Saved in the settings."}, a_triggers},
    {{"hidden", "Show hidden things", "Switches the hidden things overlay. Saved in the settings."}, a_hidden},
    {{"death", "Show death barriers", "Switches the death barriers overlay. Saved in the settings."}, a_death},
    {{"hookshot", "Show hookshot targets", "Switches the hookshot targets overlay. Saved in the settings."}, a_hook},
    {{"health", "Show enemy health", "Switches the enemy health overlay. Saved in the settings."}, a_health},
    {{"position", "Show position", "Switches the position readout."}, a_pos},
    {{"speed", "Show speed", "Switches the speed readout."}, a_speed},
    {{"area", "Show area", "Switches the area readout."}, a_area},
    {{"inputs", "Show inputs", "Switches the inputs readout."}, a_input},
    {{"timer", "Timer start / stop", "Starts or stops the timer."}, a_timer},
    {{"igt", "Speedrun timer start / stop", "Starts or pauses the speedrun timer."}, a_igt},
    {{"split", "Speedrun timer split", "Takes a split, named after the current area."}, a_split},
};
enum { N = sizeof actions / sizeof *actions };
_Static_assert(N <= 64, "dbg_bind_poll returns one bit per action");

int dbg_bind_count(void) { return N; }
const DbgBindInfo *dbg_bind_info(int i) { return i >= 0 && i < N ? &actions[i].info : NULL; }

static void setting_of(int i, char *out, size_t n) {
    snprintf(out, n, "NX2NAVI_DEBUG_HOTKEY_%s", actions[i].info.name);
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
}

static int key_named(const char *s, size_t len) {
    for (int k = KEY_NONE + 1; k < KEY_COUNT; k++) {
        const char *n = input_key_name(k);
        if (n && strlen(n) == len && !strncasecmp(n, s, len)) return k;
    }
    return KEY_NONE;
}

static int pad_named(const char *s, size_t len) {
    for (int p = PAD_NONE + 1; p < PAD_COUNT; p++) {
        const char *n = input_pad_name(p);
        if (n && strlen(n) == len && !strncasecmp(n, s, len)) return p;
    }
    return PAD_NONE;
}

// "key:g, key:f9, pad:dpad_up": entries in any order, the first two keys and the first button kept, others skipped.
static void parse(const char *v, uint8_t keys[DBG_BIND_KEYS], uint8_t *pad) {
    int nk = 0;
    keys[0] = keys[1] = 0;
    *pad = 0;
    while (v && *v) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        const char *e = v;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > v && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if (t - v > 4 && !strncasecmp(v, "key:", 4)) {
            int k = key_named(v + 4, (size_t)(t - v - 4));
            if (k && nk < DBG_BIND_KEYS && (!nk || keys[0] != k)) keys[nk++] = (uint8_t)k;
        } else if (t - v > 4 && !strncasecmp(v, "pad:", 4)) {
            int p = pad_named(v + 4, (size_t)(t - v - 4));
            if (p && !*pad) *pad = (uint8_t)p;
        }
        v = e;
    }
}

void dbg_bind_get(int i, uint8_t keys[DBG_BIND_KEYS], uint8_t *pad) {
    keys[0] = keys[1] = 0;
    *pad = 0;
    if (i < 0 || i >= N) return;
    char name[64];
    setting_of(i, name, sizeof name);
    parse(cfg_get(name), keys, pad);
}

void dbg_bind_set(int i, const uint8_t keys[DBG_BIND_KEYS], uint8_t pad) {
    if (i < 0 || i >= N) return;
    char name[64], v[96] = "";
    setting_of(i, name, sizeof name);
    size_t n = 0;
    for (int k = 0; k < DBG_BIND_KEYS; k++)
        if (keys[k] && input_key_name(keys[k]))
            n += (size_t)snprintf(v + n, sizeof v - n, "%skey:%s", n ? ", " : "", input_key_name(keys[k]));
    if (pad && input_pad_name(pad) && n < sizeof v)
        n += (size_t)snprintf(v + n, sizeof v - n, "%spad:%s", n ? ", " : "", input_pad_name(pad));
    if (n) {
        cfg_set(name, v);
    } else {   // nothing bound: the line leaves nx2navi.ini
        const char *k = name;
        cfg_reset(&k, 1);
    }
}

// The poll's own copy of every action's binds, read again when a setting changes (cfg_version).
static struct { uint8_t keys[DBG_BIND_KEYS], pad; } binds[N];
static uint32_t pads_bound;
static unsigned seen_version;
static int loaded, any_bound;

static void refresh(void) {
    unsigned v = cfg_version();
    if (loaded && v == seen_version) return;
    loaded = 1, seen_version = v, pads_bound = 0, any_bound = 0;
    for (int i = 0; i < N; i++) {
        dbg_bind_get(i, binds[i].keys, &binds[i].pad);
        if (binds[i].pad) pads_bound |= 1u << binds[i].pad;
        any_bound |= binds[i].pad || binds[i].keys[0] || binds[i].keys[1];
    }
}

uint32_t dbg_bind_pads(void) {
    refresh();
    return !pads_bound || dbg_game_menu_open() ? 0 : pads_bound;
}

uint64_t dbg_bind_poll(uint32_t pads, uint8_t *keys, int menu_open) {
    static uint8_t held[N];
    refresh();
    if (!any_bound) return 0;
    int game_menu = dbg_game_menu_open();
    uint64_t fired = 0;
    for (int i = 0; i < N; i++) {
        int down = binds[i].pad && (pads >> binds[i].pad & 1);
        for (int k = 0; k < DBG_BIND_KEYS; k++) {
            int key = binds[i].keys[k];
            if (!key || !keys) continue;
            down |= keys[key] != 0;
            if (!game_menu) keys[key] = 0;   // the game does not see it
        }
        // a press made while the menu or the game's pause menu was open and held after it closed does not count
        if (down && !held[i] && !menu_open && !game_menu) fired |= 1ull << i;
        held[i] = (uint8_t)down;
    }
    return dbg_available() ? fired : 0;
}

void dbg_bind_run(uint64_t actions_fired) {
    for (int i = 0; i < N; i++)
        if (actions_fired >> i & 1) actions[i].run();
}

int dbg_menu_button(void) {
    if (!cfg_bool("NX2NAVI_DEBUG_MENU_PAD")) return 0;
    const char *v = cfg_value("NX2NAVI_DEBUG_MENU_BUTTON");
    return v ? pad_named(v, strlen(v)) : 0;
}

void dbg_menu_button_set(int pad) {
    const char *n = pad ? input_pad_name(pad) : NULL;
    cfg_set("NX2NAVI_DEBUG_MENU_BUTTON", n ? n : "none");
}

int dbg_menu_button_closes(int pad) {
    return pad == PAD_L || pad == PAD_R || pad == PAD_LSTICK || pad == PAD_RSTICK || pad == PAD_LPADDLE1 ||
           pad == PAD_RPADDLE1 || pad == PAD_LPADDLE2 || pad == PAD_RPADDLE2;
}

// The game's bool flag Sequence_IsPause is 1 while its pause menu is open (seen in game: 1 with the Equipment page up, 0
// once it closed). Read from the flag store (debug_tools.c), which a helper thread finds on first use: until then, and
// before a save is loaded, the menu counts as closed. One read per 10 ms at most: the poll asks several times in a row.
int dbg_game_menu_open(void) {
    static uint32_t key;
    static uint64_t at;
    static int last;
    uint64_t now = plat_now_ns();
    if (at && now - at < 10000000ull) return last;
    if (!key) key = dbg_flag_key("Sequence_IsPause");
    at = now;
    last = dbg_available() && dbg_flag_get_bool(key) == 1;
    return last;
}
