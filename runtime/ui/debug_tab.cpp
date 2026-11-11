// The Debug tab of the in-game menu and the on-screen readout it switches (debug_tools.h): the player's position and
// speed, saved positions to teleport between, the game speed, a timer with laps. The tools need game build 1.1.1;
// elsewhere the tab says so and offers nothing. The texts are English only, like the mods' cheat names: this is a
// tool for testing, not part of the game's menus.
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "debug_remote.h"
#include "debug_scan.h"
#include "input.h"
#include "input_map.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "vk_debug_cam.h"
int vk_dlaa_available(void);   // vk_dlaa.c: NVIDIA DLSS came up on this PC
}

// ---- formatting

static void format_time(char *out, size_t n, uint64_t ns) {
    uint64_t ms = ns / 1000000ull;
    snprintf(out, n, "%u:%02u.%03u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000));
}

static const float kScales[] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};

static int scale_index(float s) {
    int best = 0;
    for (int i = 1; i < (int)(sizeof kScales / sizeof *kScales); i++)
        if (fabsf(kScales[i] - s) < fabsf(kScales[best] - s)) best = i;
    return best;
}

// ---- the tab

// The overlay's colours: neon ones first, picked per kind in the View group (debug_view_color_*).
static const struct { const char *name; uint8_t r, g, b; } kNeon[] = {
    {"pink", 255, 16, 240}, {"green", 57, 255, 20}, {"cyan", 0, 255, 255}, {"yellow", 255, 240, 0},
    {"orange", 255, 95, 31}, {"purple", 188, 19, 254}, {"red", 255, 30, 30}, {"blue", 40, 120, 255},
    {"white", 255, 255, 255},
};
enum { NEON = sizeof kNeon / sizeof *kNeon };
static const char *const kKindKey[DBG_VOL_KINDS] = {"NX2NAVI_DEBUG_VIEW_COLOR_WALLS", "NX2NAVI_DEBUG_VIEW_COLOR_LOADING",
                                                    "NX2NAVI_DEBUG_VIEW_COLOR_TRIGGERS", "NX2NAVI_DEBUG_VIEW_COLOR_HIDDEN",
                                                    "NX2NAVI_DEBUG_VIEW_COLOR_DEATH"};

static int neon_index(const char *name) {
    for (int i = 0; i < NEON; i++)
        if (name && !strcmp(name, kNeon[i].name)) return i;
    return 0;
}

extern "C" void tab_debug(float scale) {
    opt_begin("##debug", scale);
    if (!dbg_available()) {
        opt_text_str("These tools need game build 1.1.1: the game's memory is only mapped for that build so far.", true);
        opt_end();
        return;
    }
    static char value[160], note[160];

    opt_group_text("Player");
    float pos[3];
    bool have = dbg_player_pos(pos);
    if (have)
        snprintf(value, sizeof value, "%.2f, %.2f, %.2f", pos[0], pos[1], pos[2]);
    else
        snprintf(value, sizeof value, "no player yet");
    opt_text_row("##dbg_pos", "Position (x, y, z)", value, nullptr,
                 "The player actor's position in metres. It exists once a save has loaded.", false);
    snprintf(value, sizeof value, "%.2f m/s ground, %.2f m/s vertical", dbg_ground_speed(), dbg_vertical_speed());
    opt_text_row("##dbg_speed", "Speed", value, nullptr,
                 "Measured from the position over a tenth of a second of real time. A jump of more than 20 m (a teleport, a "
                 "loading screen) is not counted.",
                 false);
    {
        bool fly = dbg_fly_on() != 0;
        if (opt_toggle_text("##dbg_fly", "Fly (no clip)",
                            "The left stick moves where the camera looks, X (Space) rises, B (Left Ctrl) sinks, A (Left Shift) "
                            "held is three times faster. Walls and the ground do not stop the player; gravity is off.",
                            fly))
            dbg_fly(!fly);
        snprintf(value, sizeof value, "%.0f m/s", dbg_fly_speed());
        int r = opt_text_row("##dbg_flyspeed", "Fly speed", value, nullptr, "Left and right change it by 2 m/s.", false);
        if (r == -1 || r == 1) dbg_set_fly_speed(dbg_fly_speed() + 2.0f * r);
        bool hov = dbg_hover_on() != 0;
        if (opt_toggle_text("##dbg_hover", "Hover",
                            "Holds the player at the height where this was switched on: walk on air, off ledges, over gaps "
                            "and water. Switch it off to land.",
                            hov))
            dbg_hover(!hov);
        bool hb = dbg_hover_boots_on() != 0;
        if (opt_toggle_text("##dbg_hoverboots", "Infinite Hover Boots",
                            "With the Hover Boots on, a fall stops where it starts and the player keeps that height for as "
                            "long as the boots are on. B (Left Ctrl) drops.",
                            hb))
            dbg_hover_boots(!hb);
    }

    opt_group_text("Warp");
    {
        // Areas are the stage file names without the age suffix; each has entrances in one or both ages' files.
        struct Area { char name[48]; };
        static std::vector<Area> areas;
        auto base_of = [](const char *banc, char *out, size_t n) {
            size_t k = strlen(banc);
            if (k > 6 && (!strcmp(banc + k - 6, "_Child") || !strcmp(banc + k - 6, "_Adult"))) k -= 6;
            snprintf(out, n, "%.*s", (int)k, banc);
        };
        if (areas.empty())
            for (int i = 0; i < dbg_warp_count(); i++) {
                Area a;
                base_of(dbg_warp_banc(i), a.name, sizeof a.name);
                bool seen = false;
                for (auto &x : areas) seen |= !strcmp(x.name, a.name);
                if (!seen) areas.push_back(a);
            }
        static int area_i = 0, entrance_k = 0, want_child = -1;
        char cur[64] = "";
        int child = dbg_is_child();
        if (want_child < 0 && child >= 0) want_child = child;
        if (dbg_current_area(cur))
            snprintf(value, sizeof value, "%s (%s)", cur, child == 1 ? "child" : child == 0 ? "adult" : "?");
        else
            snprintf(value, sizeof value, "%s", dbg_flags_locating() ? "searching the game's memory (about half a minute)..." : "not found: load a save");
        opt_text_row("##dbg_area", "Current area", value, nullptr, "The stage file the game has loaded, and the player's age.", false);
        int sw = dbg_age_switch_target();
        snprintf(value, sizeof value, "A: %s", sw < 0 ? "-" : dbg_warp_age(sw) == DBG_AGE_CHILD ? "become a child" : "become an adult");
        if (opt_text_row("##dbg_wswitch", "Switch age here", value, nullptr,
                         "An instant warp into this area's version for the other age, at the entrance the player last came in "
                         "by (or the first one). Areas made for one age only (dungeons, most interiors) have no other version: "
                         "warp to an area that has one.",
                         false) == 2 && sw >= 0)
            dbg_age_switch();
        int rl = dbg_reload_target();
        snprintf(value, sizeof value, "A: %s", rl < 0 ? "-" : dbg_warp_start(rl));
        if (opt_text_row("##dbg_wreload", "Reload area", value, nullptr,
                         "For practice: warps back into this area at the entrance the player last came in by (or its "
                         "first one), same age, so a route or trick can be tried again from the start.",
                         false) == 2 && rl >= 0)
            dbg_reload_area();
        int hold = dbg_warp_get_age();
        int rf = opt_text_row("##dbg_wflag", "Age flag on arrival",
                         hold == DBG_AGE_CHILD ? "child" : hold == DBG_AGE_ADULT ? "adult" : "set by the area", nullptr,
                         "For glitch hunting: after a warp, the game's age flag (Sequence_IsChildhood) is held at this value "
                         "over the area's own choice. The player's body and items stay those of the area's age, so this "
                         "mixes the two: what the game lets the player do follows the flag. Left and right choose.",
                         false);
        if (rf == -1 || rf == 1) dbg_warp_set_age((hold + rf + 3) % 3);
        // The entrances of the chosen area in the chosen age (or the area's only file when it has no age suffix).
        std::vector<int> ents;
        bool has_age = false;
        for (int i = 0; i < dbg_warp_count(); i++) {
            char b[64];
            base_of(dbg_warp_banc(i), b, sizeof b);
            if (strcmp(b, areas[area_i].name)) continue;
            int age = dbg_warp_age(i);
            if (age != DBG_AGE_ANY) has_age = true;
            if (age == DBG_AGE_ANY || (age == DBG_AGE_CHILD) == (want_child == 1)) ents.push_back(i);
        }
        if (ents.empty())   // this area has no file for the chosen age: offer the other one
            for (int i = 0; i < dbg_warp_count(); i++) {
                char b[64];
                base_of(dbg_warp_banc(i), b, sizeof b);
                if (!strcmp(b, areas[area_i].name)) ents.push_back(i);
            }
        if (entrance_k >= (int)ents.size()) entrance_k = 0;
        int r = opt_text_row("##dbg_warea", "Destination", areas[area_i].name, nullptr,
                             "The area to warp to. Left and right go through every area that has an entrance.", false);
        int na = (int)areas.size();
        if (r == -1 || r == 1) area_i = (area_i + r + na) % na, entrance_k = 0;
        snprintf(value, sizeof value, "%s (%d of %d)", ents.empty() ? "-" : dbg_warp_start(ents[entrance_k]), entrance_k + 1, (int)ents.size());
        r = opt_text_row("##dbg_went", "Entrance", value, nullptr,
                         "Where in the area to arrive: the entrances other areas lead to. Left and right choose.", false);
        int ne = (int)ents.size();
        if (ne && (r == -1 || r == 1)) entrance_k = (entrance_k + r + ne) % ne;
        int pick = ne ? ents[entrance_k] : -1;
        const char *age_txt = !has_age ? (pick >= 0 ? "set by the area" : "-")
                              : dbg_warp_age(pick) == DBG_AGE_CHILD ? "child" : "adult";
        r = opt_text_row("##dbg_wage", "Age", age_txt, nullptr,
                         "Child or adult at the destination. Areas with a child and an adult version switch the player's age; "
                         "dungeons and other single-version areas keep the age the area itself asks for. Left and right switch.",
                         false);
        if (r == -1 || r == 1) {
            want_child = want_child == 1 ? 0 : 1;
            int other = dbg_warp_other_age(pick);
            entrance_k = 0;
            if (other >= 0) {
                std::vector<int> e2;
                for (int i = 0; i < dbg_warp_count(); i++)
                    if (!strcmp(dbg_warp_banc(i), dbg_warp_banc(other))) e2.push_back(i);
                for (int k = 0; k < (int)e2.size(); k++)
                    if (e2[k] == other) entrance_k = k;
            }
        }
        if (opt_text_row("##dbg_wnow", "Warp now", "A: go", nullptr,
                         "Puts the player into the nearest loading zone of this area and sends that area change to the "
                         "destination. Works while the player is free to move (not in a cutscene or a menu of the game).",
                         false) == 2 && pick >= 0) {
            if (dbg_warp_arm(pick)) dbg_warp_now();
        }
        if (opt_text_row("##dbg_wnext", "Warp at the next area change", "A: arm", nullptr,
                         "The next area change the game makes on its own (a loading zone, a door, a warp song, Farore's "
                         "Wind, a void out) goes to the destination instead.", false) == 2 && pick >= 0)
            dbg_warp_arm(pick);
        if (dbg_warp_armed() >= 0 &&
            opt_text_row("##dbg_wcancel", "Cancel the warp", "A", nullptr, "Lets the next area change go where the game sends it.", false) == 2)
            dbg_warp_cancel();
        if (dbg_warp_note()[0]) opt_text_str(dbg_warp_note());
    }

    opt_group_text("Health");
    {
        int c = 0, m = 0;
        bool ok = dbg_health_get(&c, &m) != 0;
        if (ok)
            snprintf(value, sizeof value, "%.2f of %.2f hearts", c / 4.0, m / 4.0);
        else
            snprintf(value, sizeof value, "%s", dbg_health_locating() ? "searching the game's memory (about half a minute)..." : "not found: load a save");
        opt_text_row("##dbg_health", "Health", value, nullptr,
                     "Read from the game's flag store, found by its key on first use. A quarter heart is one unit.", false);
        bool god = dbg_invulnerable_on() != 0;
        if (opt_toggle_text("##dbg_god", "Invulnerable", "Raises health back to full on every frame. A hit that would kill in a single "
                            "frame can still get through.", god))
            dbg_invulnerable(!god);
        {
            // The value to hold, in quarter hearts: whole hearts on one row, single quarters on the next (12, 12.25, 12.5, 12.75 ...).
            static int target = 0;
            int lk = dbg_health_lock_value();
            if (!target) target = lk ? lk : (ok && c ? c : 48);
            snprintf(value, sizeof value, "%.2f hearts, %s", target / 4.0, lk ? "holding" : "off");
            int r = opt_text_row("##dbg_lock", "Hold health at", value, nullptr,
                                 "Keeps health at exactly this value on every frame, whatever damage or healing happens: for "
                                 "testing a fixed amount of health. Left and right change it by a whole heart; A switches the hold "
                                 "on or off. The invulnerable switch above only keeps health full. A value above the maximum "
                                 "holds the maximum.", false);
            if (r == -1) target = target > 4 ? target - 4 : 1;
            if (r == 1) target = target + 4 > 80 ? 80 : target + 4;
            if (r == 2) lk = lk ? 0 : target, dbg_health_lock(lk);
            snprintf(value, sizeof value, "%d quarter%s (%.2f hearts)", target, target == 1 ? "" : "s", target / 4.0);
            int q = opt_text_row("##dbg_lockq", "Hold health: fine adjust", value, nullptr,
                                 "Left and right change the value to hold by one quarter heart, so 12, 12.25, 12.5, 12.75 "
                                 "hearts and so on can be set.", false);
            if (q == -1 && target > 1) target--;
            if (q == 1 && target < 80) target++;
            if (r == 0 && q == 0) {
                if (lk) target = lk;                                  // a value set elsewhere (the console) is adopted
            } else if (dbg_health_lock_value()) {
                dbg_health_lock(target);                              // changed while holding: follow it at once
            }
        }
        if (opt_text_row("##dbg_refill", "Refill health", "A: full", nullptr, "Sets health to the current maximum.", false) == 2 && ok)
            dbg_health_set(m);
        if (opt_text_row("##dbg_onehp", "Set to one quarter heart", "A: set", nullptr, "For testing low health (damage warning, death).", false) == 2 && ok)
            dbg_health_set(1);
        if (opt_text_row("##dbg_addheart", "Add a heart container", "A: +1", nullptr, "Raises the maximum by one heart (up to 20) and refills.", false) == 2 && ok) {
            dbg_health_set_max(m + 4);
            dbg_health_set(m + 4);
        }
        if (opt_text_row("##dbg_subheart", "Remove a heart container", "A: -1", nullptr, "Lowers the maximum by one heart (down to 1).", false) == 2 && ok)
            dbg_health_set_max(m - 4);
    }

    opt_group_text("Magic and items");
    {
        const char *wait = dbg_flags_locating() ? "searching the game's memory (about half a minute)..." : "not found: load a save";
        int c = 0, m = 0;
        bool ok = dbg_magic_get(&c, &m) != 0;
        if (ok && m > 0)
            snprintf(value, sizeof value, "%d of %d", c, m);
        else
            snprintf(value, sizeof value, "%s", ok ? "no magic meter yet" : wait);
        int r = opt_text_row("##dbg_magic", "Magic", value, nullptr,
                             "The player's magic meter. Left and right change it by a tenth of the maximum; A fills it.", false);
        if (ok && m > 0 && r) dbg_magic_set(r == 2 ? m : c + r * (m / 10 > 0 ? m / 10 : 1));
        int lvl = dbg_magic_level();
        r = opt_text_row("##dbg_magiclvl", "Magic meter",
                         lvl == 0 ? "none" : lvl == 1 ? "single" : lvl == 2 ? "double" : wait, nullptr,
                         "No magic, the single meter or the double meter (the great fairies' gifts). Left and right change it.",
                         false);
        if (lvl >= 0 && (r == -1 || r == 1)) dbg_magic_set_level((lvl + r + 3) % 3);
        bool inf = dbg_magic_infinite_on() != 0;
        if (opt_toggle_text("##dbg_infmagic", "Infinite magic", "Fills the magic meter again on every frame.", inf)) dbg_magic_infinite(!inf);
        bool infi = dbg_items_infinite_on() != 0;
        if (opt_toggle_text("##dbg_infitems", "Infinite items", "Keeps rupees, arrows, bombs, bombchus, Deku sticks, nuts and seeds and "
                            "magic beans at their largest value on every frame, so nothing runs out. Gold Skulltula tokens and "
                            "small keys are not touched. Use Infinite magic above for the magic meter.", infi))
            dbg_items_infinite(!infi);
        for (int i = 0; i < dbg_item_count(); i++) {
            char id[32];
            int v = 0, max = 0, step = 1;
            const char *label = dbg_item_label(i, &max, &step);
            bool have = dbg_item_get(i, &v) != 0;
            snprintf(id, sizeof id, "##dbg_item%d", i);
            if (have)
                snprintf(value, sizeof value, "%d", v);
            else
                snprintf(value, sizeof value, "%s", wait);
            r = opt_text_row(id, label, value, nullptr,
                             "Written to the game's own counter. Left and right change it by one step; A sets the largest value. "
                             "The game's capacity (wallet, quiver, bag) is not checked, and a count for an item that was never "
                             "obtained does not add the item.", false);
            if (have && r) dbg_item_set(i, r == 2 ? max : v + r * step);
        }
    }

    opt_group_text("Owned items");
    {
        // One group of things at a time (equipment, items, upgrades, quest, songs, masks): the group row chooses.
        static const char *groups[] = {"Equipment", "Items", "Upgrades", "Quest", "Songs", "Masks"};
        const int ng = (int)(sizeof groups / sizeof groups[0]);
        static int gi = 0;
        int r = opt_text_row("##dbg_owngroup", "Show", groups[gi], nullptr, "Which things to list. Left and right choose.", false);
        if (r == -1 || r == 1) gi = (gi + r + ng) % ng;
        int all_on = -1;
        if (opt_text_row("##dbg_ownall", "Give all of these", "A", nullptr, "Gives every thing listed below.", false) == 2) all_on = 1;
        if (opt_text_row("##dbg_ownnone", "Remove all of these", "A", nullptr, "Removes every thing listed below.", false) == 2) all_on = 0;
        for (int i = 0; i < dbg_owned_count(); i++) {
            const char *group = nullptr, *label = dbg_owned_label(i, &group);
            if (strcmp(group, groups[gi])) continue;
            if (all_on >= 0) dbg_owned_set(i, all_on);
            int have = dbg_owned_get(i);
            char id[32];
            snprintf(id, sizeof id, "##dbg_own%d", i);
            if (have < 0) {
                opt_text_row(id, label, dbg_flags_locating() ? "searching..." : "not found", nullptr, "", false);
                continue;
            }
            if (opt_toggle_text(id, label,
                                "Gives or removes it through the game's own item flags (the pouch count and the got-it flag). "
                                "Removing something that is equipped leaves it in the hand until the next change.",
                                have == 1))
                dbg_owned_set(i, have != 1);
        }
    }

    opt_group_text("Saved positions");
    opt_text_str("Saves only where the player stands, not the game's state. The game may pull the actor back if its "
                 "physics body disagrees: loading is experimental.");
    for (int i = 0; i < DBG_SLOTS; i++) {
        char id[32], label[48];
        float p[3];
        bool used = dbg_slot_used(i, p);
        snprintf(id, sizeof id, "##dbg_save%d", i);
        snprintf(label, sizeof label, "Save position to slot %d", i + 1);
        if (used)
            snprintf(value, sizeof value, "%.1f, %.1f, %.1f", p[0], p[1], p[2]);
        else
            snprintf(value, sizeof value, "empty");
        if (opt_text_row(id, label, value, nullptr, "Keeps the player's current position in this slot.", false) == 2) dbg_slot_save(i);
        snprintf(id, sizeof id, "##dbg_load%d", i);
        snprintf(label, sizeof label, "Go to slot %d", i + 1);
        if (opt_text_row(id, label, used ? "A: teleport" : "empty", nullptr,
                         "Moves the player to the position saved in this slot.", false) == 2 && used)
            dbg_slot_load(i);
    }

    opt_group_text("Game speed");
    {
        float s = dbg_time_scale();
        snprintf(value, sizeof value, "%.3gx", s);
        snprintf(note, sizeof note, "slowest about 1/8 at 60 fps, fastest about 4x");
        int r = opt_text_row("##dbg_scale", "Game speed", value, note,
                             "Scales the time step the game is given each frame: slow motion to study a trick, fast "
                             "forward to skip waiting. Left and right change it. Physics limits how slow it can go.",
                             false);
        int idx = scale_index(s);
        if (r == -1 && idx > 0) dbg_set_time_scale(kScales[idx - 1]);
        if (r == 1 && idx + 1 < (int)(sizeof kScales / sizeof *kScales)) dbg_set_time_scale(kScales[idx + 1]);
        if (r == 2) dbg_set_time_scale(1.0f);
    }

    opt_group_text("View");
    {
        // Settings (nx2navi.ini [debug]) that the renderer reads at once.
        static const struct { const char *key, *id, *label, *desc; } sw[] = {
            {"NX2NAVI_DEBUG_FOG_OFF", "##dbg_fog_off", "Fog off",
             "Removes the volumetric fog: the haze of the Lost Woods, Kokiri Forest's mist and the distance fog over "
             "Hyrule Field. Saved in the settings."},
        };
        for (const auto &s : sw) {
            bool on = cfg_bool(s.key) != 0;
            if (opt_toggle_text(s.id, s.label, s.desc, on)) {
                cfg_set(s.key, on ? "0" : "1");
                opt_save_soon();
            }
        }
        // The best picture the renderer's own settings give (the Screen and Graphics tabs set each one): a 4K game
        // picture, the 3D scene drawn at its full size with DLAA (FSR 3.1 Native AA where DLSS cannot run; without
        // either the scene is 75% of the picture, widescreen.c scene_share), the sharpest sun shadows, 16x texture
        // filtering and full texture detail. Most are read at start, so a restart applies them. Render scale and shadow
        // range are left out: game build 1.1.1 has no scene or cascade patch set (resolution.c res_init).
        static const struct { const char *key, *best; } q[] = {
            {"NX2NAVI_GAME_RES", "2160"}, {"NX2NAVI_DLAA", "1"}, {"NX2NAVI_DLSS", "dlaa"},
            {"NX2NAVI_SHADOW_SCALE", "4"}, {"NX2NAVI_ANISO", "16"}, {"NX2NAVI_TEXTURE_DETAIL", "full"},
        };
        enum { NQ = sizeof q / sizeof *q };
        int best = 0;
        for (const auto &e : q) {
            const char *v = cfg_value(e.key);
            best += v && !strcmp(v, e.best);
        }
        snprintf(value, sizeof value, "%d of %d set%s", best, (int)NQ, cfg_pending_count() ? ", restart to apply" : "");
        if (opt_text_row("##dbg_quality_max", "Best quality", value, nullptr,
                         "Sets the game picture to 4K (2160p), draws the 3D scene at its full size with DLAA (or AMD "
                         "FSR 3.1 Native AA where DLSS cannot run) instead of 75% of it, the sharpest sun shadows (4x), "
                         "16x texture filtering and full texture detail. Needs a fast GPU. Most of these apply after a "
                         "restart.",
                         false) == 2) {
            for (const auto &e : q) opt_apply(e.key, e.best);
            if (!vk_dlaa_available()) opt_apply("NX2NAVI_TAA", "fsr3");
        }
        if (opt_text_row("##dbg_quality_game", "Game's quality", "A", nullptr,
                         "Puts the settings above back to their defaults (1440p, no DLAA, the game's shadows and "
                         "filtering).",
                         false) == 2) {
            const char *keys[NQ];
            for (int i = 0; i < NQ; i++) keys[i] = q[i].key + 8;   // without NX2NAVI_
            cfg_reset(keys, NQ);
            opt_save_soon();
        }
        {
            // Foliage draw distance (draw_distance.c): read before the game starts, so it applies at the next start.
            static const char *const steps[] = {"0.25", "0.5", "1", "1.5", "2", "3", "4", "6", "8"};
            enum { NS = sizeof steps / sizeof *steps };
            double now = cfg_double("NX2NAVI_DEBUG_DRAW_DISTANCE");
            int si = 0;
            for (int i = 1; i < NS; i++)
                if (fabs(atof(steps[i]) - now) < fabs(atof(steps[si]) - now)) si = i;
            snprintf(value, sizeof value, "x%s (grass %.0f m)%s", steps[si], 40 * atof(steps[si]),
                     cfg_pending("NX2NAVI_DEBUG_DRAW_DISTANCE") ? ", restart to apply" : "");
            int r = opt_text_row("##dbg_drawdist", "Plant draw distance", value, nullptr,
                                 "How far out grass tufts, bushes, flowers and low trees are drawn, as a multiple of the "
                                 "game's distances (grass 40 m, low trees 135 m), in the areas that keep the game's own "
                                 "(most of them). Higher shows "
                                 "plants further away; lower draws less, for speed. Takes effect at the next start. "
                                 "Left and right change it.",
                                 false);
            if (r == -1 || r == 1) {
                si = si + r < 0 ? 0 : si + r >= NS ? NS - 1 : si + r;
                opt_apply("NX2NAVI_DEBUG_DRAW_DISTANCE", steps[si]);
            }
        }

        // The volume overlay (debug_overlay_draw): settings, so they stay as set.
        static const struct { const char *key, *id, *label, *desc; } vs[] = {
            {"NX2NAVI_DEBUG_VIEW_WALLS", "##dbg_v_walls", "Show invisible walls",
             "Draws the invisible walls placed as objects (air walls), seen through everything. The walls inside the "
             "level's own collision mesh are not included yet."},
            {"NX2NAVI_DEBUG_VIEW_LOADING", "##dbg_v_load", "Show loading zones",
             "Draws the trigger boxes that load another area when the player walks in."},
            {"NX2NAVI_DEBUG_VIEW_TRIGGERS", "##dbg_v_trig", "Show trigger zones",
             "Draws every other trigger volume: event, camera, music and spawn areas, horse jump limits and the like."},
            {"NX2NAVI_DEBUG_VIEW_HIDDEN", "##dbg_v_hidden", "Show hidden things",
             "Marks hidden holes (grottos), Gold Skulltulas, pieces of heart, chests and magic bean spots."},
            {"NX2NAVI_DEBUG_VIEW_DEATH", "##dbg_v_death", "Show death barriers",
             "Draws what kills or sends the player back: the kill volume under the world (fall out of the level and "
             "it catches you), lava surfaces, and marks bottomless pits and quicksand."},
        };
        for (const auto &s : vs) {
            bool on = cfg_bool(s.key) != 0;
            if (opt_toggle_text(s.id, s.label, s.desc, on)) {
                cfg_set(s.key, on ? "0" : "1");
                opt_save_soon();
            }
        }
        static const char *const styles[] = {"wire", "solid", "both"};
        static const char *const style_names[] = {"Wireframe", "Solid colour", "Both"};
        int si = 0;
        while (si < 2 && strcmp(cfg_value("NX2NAVI_DEBUG_VIEW_STYLE"), styles[si])) si++;
        int r = opt_text_row("##dbg_v_style", "Overlay style", style_names[si], nullptr,
                             "Wireframe draws the edges; solid colour fills the faces see-through (Opacity); both does both. "
                             "Left and right change it.",
                             false);
        if (r) {
            si = (si + (r == -1 ? 2 : 1)) % 3;
            cfg_set("NX2NAVI_DEBUG_VIEW_STYLE", styles[si]);
            opt_save_soon();
        }
        int op = cfg_int("NX2NAVI_DEBUG_VIEW_OPACITY");
        snprintf(value, sizeof value, "%d %%", op);
        r = opt_text_row("##dbg_v_op", "Solid colour opacity", value, nullptr,
                         "How strongly a solid face covers what is behind it. Left and right change it.", false);
        if (r == -1 || r == 1) {
            op += r * 10;
            op = op < 10 ? 10 : op > 100 ? 100 : op;
            snprintf(value, sizeof value, "%d", op);
            cfg_set("NX2NAVI_DEBUG_VIEW_OPACITY", value);
            opt_save_soon();
        }
        static const char *const kind_names[DBG_VOL_KINDS] = {"Colour: invisible walls", "Colour: loading zones",
                                                              "Colour: trigger zones", "Colour: hidden things",
                                                              "Colour: death barriers"};
        for (int k = 0; k < DBG_VOL_KINDS; k++) {
            int ci = neon_index(cfg_value(kKindKey[k]));
            char id[32];
            snprintf(id, sizeof id, "##dbg_v_col%d", k);
            r = opt_text_row(id, kind_names[k], kNeon[ci].name, nullptr,
                             "Neon pink, green, cyan, yellow, orange or purple, or red, blue or white. Left and right "
                             "change it.",
                             false);
            if (r) {
                ci = (ci + (r == -1 ? NEON - 1 : 1)) % NEON;
                cfg_set(kKindKey[k], kNeon[ci].name);
                opt_save_soon();
            }
        }
        {
            bool on = cfg_bool("NX2NAVI_DEBUG_VIEW_LABELS") != 0;
            if (opt_toggle_text("##dbg_v_labels", "Names on the overlay", "Writes each volume's name over it, within 60 m.",
                                on)) {
                cfg_set("NX2NAVI_DEBUG_VIEW_LABELS", on ? "0" : "1");
                opt_save_soon();
            }
        }
        static const int ranges[] = {25, 50, 100, 150, 250, 500, 1000};
        int rg = cfg_int("NX2NAVI_DEBUG_VIEW_RANGE"), ri = 0;
        while (ri < 6 && ranges[ri] < rg) ri++;
        snprintf(value, sizeof value, "%d m", ranges[ri]);
        r = opt_text_row("##dbg_v_range", "Overlay range", value, nullptr,
                         "Volumes further from the player than this are not drawn. Left and right change it.", false);
        if (r == -1 || r == 1) {
            ri += r;
            ri = ri < 0 ? 0 : ri > 6 ? 6 : ri;
            snprintf(value, sizeof value, "%d", ranges[ri]);
            cfg_set("NX2NAVI_DEBUG_VIEW_RANGE", value);
            opt_save_soon();
        }
    }

    opt_group_text("Timer");
    {
        char t[32], ticks[32];
        format_time(t, sizeof t, dbg_timer_ns());
        snprintf(ticks, sizeof ticks, "%llu frames", (unsigned long long)dbg_timer_ticks());
        snprintf(value, sizeof value, "%s%s  (%s)", dbg_timer_running() ? "running " : "stopped ", t, ticks);
        if (opt_text_row("##dbg_timer", "Start / stop", value, nullptr,
                         "Times real seconds and counts the game's own 60 Hz frames over the same span.", false) == 2)
            dbg_timer_toggle();
        if (opt_text_row("##dbg_lap", "Lap", "", nullptr, "Keeps the current time as a split.", false) == 2) dbg_timer_lap();
        if (opt_text_row("##dbg_reset", "Reset timer", "", nullptr, "Stops the timer and clears it and the laps.", false) == 2)
            dbg_timer_reset();
        uint64_t laps[DBG_LAPS];
        int n = dbg_timer_laps(laps);
        for (int i = 0; i < n; i++) {
            char lt[32], line[64];
            format_time(lt, sizeof lt, laps[i]);
            snprintf(line, sizeof line, "Lap %d: %s", i + 1, lt);
            opt_text_str(line);
        }
    }

    opt_group_text("Speedrun timer");
    {
        // The three switches are settings (nx2navi.ini [debug]), so they stay as set.
        static const struct { const char *key, *id, *label, *desc; } sw[] = {
            {"NX2NAVI_DEBUG_IGT", "##dbg_igt_on", "Show speedrun timer",
             "In-game time (real time without the loading screens), real time and the splits, at the top right of the screen. "
             "Saved in the settings."},
            {"NX2NAVI_DEBUG_IGT_AUTOSPLIT", "##dbg_igt_as", "Split on every area change",
             "Takes a split each time the game loads another area, named after the area left. Saved in the settings."},
            {"NX2NAVI_DEBUG_IGT_AUTOSTART", "##dbg_igt_ast", "Start when a save loads",
             "Starts the timer by itself the moment a save's player first appears (from the title screen or a reset). "
             "Saved in the settings."},
        };
        for (const auto &s : sw) {
            bool on = cfg_bool(s.key) != 0;
            if (opt_toggle_text(s.id, s.label, s.desc, on)) {
                cfg_set(s.key, on ? "0" : "1");
                opt_save_soon();
            }
        }
        uint64_t igt = 0, rta = 0;
        dbg_igt_times(&igt, &rta);
        char a[32], b[32];
        format_time(a, sizeof a, igt);
        format_time(b, sizeof b, rta);
        snprintf(value, sizeof value, "%s IGT %s  RTA %s", dbg_igt_running() ? "running" : "stopped", a, b);
        if (opt_text_row("##dbg_igt_go", "Start / stop (F2)", value, nullptr,
                         "Starts or pauses the speedrun timer. With the hotkeys on, F2 does the same, F3 splits and F4 resets.",
                         false) == 2)
            dbg_igt_toggle();
        if (opt_text_row("##dbg_igt_split", "Split (F3)", "A", nullptr, "Takes a split now, named after the current area.", false) == 2)
            dbg_igt_split();
        if (opt_text_row("##dbg_igt_reset", "Reset (F4)", "A", nullptr,
                         "Clears the timer and the splits. A stopped run that is faster than the best run (or the first run) "
                         "becomes the best run, kept in debug_splits.txt in the game's folder; splits are compared with it.",
                         false) == 2)
            dbg_igt_reset();
        snprintf(value, sizeof value, "%d splits", dbg_igt_best_count());
        if (dbg_igt_best_count() &&
            opt_text_row("##dbg_igt_clear", "Forget the best run", value, nullptr, "Deletes the kept best run.", false) == 2)
            dbg_igt_clear_best();
        for (int i = 0; i < dbg_igt_splits(); i++) {
            uint64_t ns;
            int64_t d;
            int hd;
            char name[48], line[128], dt[32];
            if (!dbg_igt_split_at(i, &ns, name, &d, &hd)) break;
            format_time(a, sizeof a, ns);
            format_time(dt, sizeof dt, (uint64_t)(d < 0 ? -d : d));
            snprintf(line, sizeof line, "%d. %s  %s%s%s", i + 1, name, a, hd ? (d < 0 ? "  -" : "  +") : "", hd ? dt : "");
            opt_text_str(line);
        }
    }

    opt_group_text("On-screen readout");
    {
        int mask = dbg_overlay_mask();
        struct {
            int bit;
            const char *id, *label, *desc;
        } rows[] = {
            {DBG_SHOW_POS, "##dbg_o_pos", "Show position", "Draws the player's position in the top left corner."},
            {DBG_SHOW_SPEED, "##dbg_o_speed", "Show speed", "Draws the ground and vertical speed."},
            {DBG_SHOW_TIMER, "##dbg_o_timer", "Show timer", "Draws the timer and the last lap."},
            {DBG_SHOW_AREA, "##dbg_o_area", "Show area", "Draws the area's stage file and the entrance the player came in by: what Reload area (F12) goes back to."},
            {DBG_SHOW_INPUT, "##dbg_o_input", "Show inputs", "Draws the buttons and sticks the game is being given, bottom left: for checking a route or an input sequence."},
        };
        for (auto &r : rows)
            if (opt_toggle_text(r.id, r.label, r.desc, (mask & r.bit) != 0)) mask ^= r.bit;
        dbg_overlay_set(mask);
        bool hk = dbg_hotkeys() != 0;
        if (opt_toggle_text("##dbg_hotkeys", "Keyboard hotkeys", "With the menu closed: F5 saves the position in slot 1, F6 goes to it, "
                            "F7 and F8 lower and raise the game speed, F9 starts and stops the timer, F12 reloads the area. "
                            "The speedrun timer uses F2 (start, pause), F3 (split) and F4 (reset).", hk))
            dbg_hotkeys_set(!hk);
    }

    opt_group_text("Remote console");
    {
        bool on = dbg_remote_enabled() != 0;
        if (opt_toggle_text("##dbg_remote", "Remote console (files)", "Lets a helper tool read and write the game's memory by "
                            "dropping commands in debug_cmd.txt next to the game; answers appear in debug_out.txt. No network "
                            "port is opened. Turn it off when you are done.", on))
            dbg_remote_set(!on);
    }

    opt_group_text("Memory scanner");
    {
        static int32_t target = 0;
        static const int32_t kSteps[] = {1, 10, 100, 1000};
        static int step_i = 0;
        static char msg[96] = "";
        int step = kSteps[step_i];
        snprintf(value, sizeof value, "%d", (int)target);
        int r = opt_text_row("##scan_val", "Value to look for", value, nullptr,
                             "Left and right change the value by the step; A sets it to 0. Search for a number you can see in "
                             "game (rupees, hearts), change it in game, then narrow the hits.", false);
        if (r == -1) target -= step;
        if (r == 1) target += step;
        if (r == 2) target = 0;
        snprintf(value, sizeof value, "%d", step);
        if (opt_text_row("##scan_step", "Step", value, nullptr, "A cycles 1, 10, 100, 1000.", false) == 2) step_i = (step_i + 1) % 4;
        if (opt_text_row("##scan_first", "New search", "A", nullptr, "Searches all game memory for this value (a second or two).", false) == 2)
            snprintf(msg, sizeof msg, "%d hits%s", dbg_scan_first(target), dbg_scan_truncated() ? " (table full)" : "");
        static const struct { int mode; const char *id, *label; } nx[] = {
            {SCAN_EQUAL, "##scan_eq", "Keep: equals the value"}, {SCAN_CHANGED, "##scan_ch", "Keep: changed"},
            {SCAN_UNCHANGED, "##scan_un", "Keep: unchanged"}, {SCAN_INCREASED, "##scan_up", "Keep: increased"},
            {SCAN_DECREASED, "##scan_dn", "Keep: decreased"}};
        for (auto &n : nx)
            if (opt_text_row(n.id, n.label, "A", nullptr, "Narrows the hits by what the game has done since the last search.", false) == 2)
                snprintf(msg, sizeof msg, "%d hits", dbg_scan_next(n.mode, target));
        if (opt_text_row("##scan_clear", "Clear hits", "A", nullptr, "Forgets the hits.", false) == 2) {
            dbg_scan_reset();
            snprintf(msg, sizeof msg, "cleared");
        }
        if (opt_text_row("##scan_dump", "Save hits to debug_scan.txt", "A", nullptr,
                         "Writes up to 4096 hits, with their offsets from the main module, next to the game.", false) == 2)
            snprintf(msg, sizeof msg, "%s", dbg_scan_dump() ? "saved debug_scan.txt" : "could not write the file");
        if (msg[0]) opt_text_str(msg);
        int n = dbg_scan_count();
        for (int i = 0; i < n && i < 12; i++) {
            uint64_t a;
            int32_t v;
            char id[32], label[64];
            if (!dbg_scan_get(i, &a, &v)) break;
            snprintf(id, sizeof id, "##scan_hit%d", i);
            snprintf(label, sizeof label, "%llx (main+%llx)", (unsigned long long)a, (unsigned long long)(a - MOD_MAIN_BASE));
            snprintf(value, sizeof value, "%d", (int)v);
            if (opt_text_row(id, label, value, nullptr, "A writes the value to look for into this address.", false) == 2)
                dbg_scan_write(i, target);
        }
        if (n > 12) opt_text_str("Only the first 12 hits are listed: narrow them further or save the file.");
    }
    opt_end();
}

// ---- the readout, top left, over the game

// ---- volume overlay (View group): invisible walls, loading zones, triggers and hidden things drawn over the picture
// with the scene camera (vk_debug_cam.c), as wireframe, solid colour or both. Nothing hides them: they show through
// walls, which is what finding them needs.

static int kind_slot(int kind) {
    return kind == DBG_VOL_WALL ? 0 : kind == DBG_VOL_LOAD ? 1 : kind == DBG_VOL_TRIGGER ? 2 : kind == DBG_VOL_HIDDEN ? 3 : 4;
}

// The scene camera and the rectangle of the window the 3D picture fills (the game's aspect, fitted in the window).
struct ViewProj {
    float vp[16];
    float x0, y0, w, h;
};

static const float kNear = 0.05f;   // clip-space w below this is behind or at the eye

static void clip_of(const ViewProj &P, const float p[3], float c[4]) {
    for (int r = 0; r < 4; r++) c[r] = P.vp[r * 4] * p[0] + P.vp[r * 4 + 1] * p[1] + P.vp[r * 4 + 2] * p[2] + P.vp[r * 4 + 3];
}

static ImVec2 screen_of(const ViewProj &P, const float c[4]) {
    float x = c[0] / c[3], y = c[1] / c[3];
    return ImVec2(P.x0 + (x * 0.5f + 0.5f) * P.w, P.y0 + (0.5f - y * 0.5f) * P.h);
}

static void lerp4(const float a[4], const float b[4], float t, float o[4]) {
    for (int i = 0; i < 4; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}

static void draw_edge(ImDrawList *dl, const ViewProj &P, const float a[4], const float b[4], ImU32 col, float th) {
    if (a[3] < kNear && b[3] < kNear) return;
    float pa[4], pb[4];
    memcpy(pa, a, sizeof pa), memcpy(pb, b, sizeof pb);
    if (pa[3] < kNear) lerp4(a, b, (kNear - a[3]) / (b[3] - a[3]), pa);
    if (pb[3] < kNear) lerp4(b, a, (kNear - b[3]) / (a[3] - b[3]), pb);
    dl->AddLine(screen_of(P, pa), screen_of(P, pb), col, th);
}

// A convex face (n corners in clip space), cut at the near plane, filled.
static void draw_face(ImDrawList *dl, const ViewProj &P, const float (*c)[4], int n, ImU32 col) {
    float out[40][4];
    int m = 0;
    for (int i = 0; i < n && m < 38; i++) {
        const float *a = c[i], *b = c[(i + 1) % n];
        bool ina = a[3] >= kNear, inb = b[3] >= kNear;
        if (ina) memcpy(out[m++], a, sizeof out[0]);
        if (ina != inb) lerp4(a, b, (kNear - a[3]) / (b[3] - a[3]), out[m++]);
    }
    if (m < 3) return;
    ImVec2 pts[40];
    for (int i = 0; i < m; i++) pts[i] = screen_of(P, out[i]);
    dl->AddConvexPolyFilled(pts, m, col);
}

// World point of a volume's local point: x and z across the middle, y up from the actor's position (its floor), all
// in units of the scale.
static void world_of(const DbgVolume &v, float lx, float ly, float lz, float o[3]) {
    float s[3] = {lx * v.scale[0], ly * v.scale[1], lz * v.scale[2]};
    for (int i = 0; i < 3; i++) o[i] = v.pos[i] + v.axes[i] * s[0] + v.axes[3 + i] * s[1] + v.axes[6 + i] * s[2];
}

static void draw_volume(ImDrawList *dl, const ViewProj &P, const DbgVolume &v, int style, ImU32 line, ImU32 fill,
                        float th) {
    DbgVolume mk = v;
    if (v.shape == DBG_SHAPE_MARKER) {   // an object: a 0.8 m cube round it, upright
        static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        memcpy(mk.axes, id, sizeof id);
        mk.scale[0] = mk.scale[1] = mk.scale[2] = 0.8f;
    }
    const DbgVolume &u = mk;
    if (v.shape == DBG_SHAPE_CYLINDER) {
        enum { SEG = 20 };
        float ring[2][SEG][4];
        for (int k = 0; k < SEG; k++) {
            float a = 6.2831853f * k / SEG, w[3];
            for (int y = 0; y < 2; y++) world_of(u, 0.5f * cosf(a), (float)y, 0.5f * sinf(a), w), clip_of(P, w, ring[y][k]);
        }
        if (style & 2) {
            float cap[SEG][4];
            for (int y = 0; y < 2; y++) {
                for (int k = 0; k < SEG; k++) memcpy(cap[k], ring[y][k], sizeof cap[0]);
                draw_face(dl, P, cap, SEG, fill);
            }
            for (int k = 0; k < SEG; k++) {
                float q[4][4];
                memcpy(q[0], ring[0][k], 16), memcpy(q[1], ring[0][(k + 1) % SEG], 16);
                memcpy(q[2], ring[1][(k + 1) % SEG], 16), memcpy(q[3], ring[1][k], 16);
                draw_face(dl, P, q, 4, fill);
            }
        }
        if (style & 1)
            for (int k = 0; k < SEG; k++) {
                for (int y = 0; y < 2; y++) draw_edge(dl, P, ring[y][k], ring[y][(k + 1) % SEG], line, th);
                if (k % 5 == 0) draw_edge(dl, P, ring[0][k], ring[1][k], line, th);
            }
        return;
    }
    float c[8][4];
    for (int i = 0; i < 8; i++) {
        float w[3];
        world_of(u, (i & 1) ? 0.5f : -0.5f, (i & 2) ? 1.0f : 0.0f, (i & 4) ? 0.5f : -0.5f, w);
        clip_of(P, w, c[i]);
    }
    if (v.kind == DBG_VOL_DEATH) {
        // A lava surface or the kill volume is a plate tens to thousands of metres wide: filled, it would cover the
        // whole picture from above, so it is drawn as its outline and a grid over its top, the surface that kills, in
        // any style.
        static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto &q : e) draw_edge(dl, P, c[q[0]], c[q[1]], line, th);
        for (int axis = 0; axis < 2; axis++) {
            float size = v.scale[axis == 0 ? 0 : 2];
            int n = (int)(size / 5.0f);
            n = n < 4 ? 4 : n > 24 ? 24 : n;
            for (int k = 1; k < n; k++) {
                float t = -0.5f + (float)k / n, a[3], b[3], ca[4], cb[4];
                if (axis == 0) world_of(u, t, 1, -0.5f, a), world_of(u, t, 1, 0.5f, b);
                else world_of(u, -0.5f, 1, t, a), world_of(u, 0.5f, 1, t, b);
                clip_of(P, a, ca), clip_of(P, b, cb);
                draw_edge(dl, P, ca, cb, line, th * 0.6f);
            }
        }
        return;
    }
    if (style & 2) {
        static const int f[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
        for (const auto &q : f) {
            float fc[4][4];
            for (int k = 0; k < 4; k++) memcpy(fc[k], c[q[k]], sizeof fc[0]);
            draw_face(dl, P, fc, 4, fill);
        }
    }
    if (style & 1) {
        static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto &q : e) draw_edge(dl, P, c[q[0]], c[q[1]], line, th);
    }
}

// The eye: the point where clip x, y and w are all 0 (rows 0, 1 and 3 of the view-projection), by Cramer's rule.
static bool eye_of(const ViewProj &P, float e[3]) {
    const float *a = P.vp, *b = P.vp + 4, *c = P.vp + 12;
    float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    if (fabsf(det) < 1e-9f) return false;
    float r[3] = {-a[3], -b[3], -c[3]};
    e[0] = (r[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r[1] * c[2] - b[2] * r[2]) + a[2] * (r[1] * c[1] - b[1] * r[2])) / det;
    e[1] = (a[0] * (r[1] * c[2] - b[2] * r[2]) - r[0] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * r[2] - r[1] * c[0])) / det;
    e[2] = (a[0] * (b[1] * r[2] - r[1] * c[1]) - a[1] * (b[0] * r[2] - r[1] * c[0]) + r[0] * (b[0] * c[1] - b[1] * c[0])) / det;
    return true;
}

// p inside the volume (box or cylinder; an object's marker never holds the eye)
static bool inside(const DbgVolume &v, const float p[3]) {
    if (v.shape == DBG_SHAPE_MARKER) return false;
    float d[3] = {p[0] - v.pos[0], p[1] - v.pos[1], p[2] - v.pos[2]}, l[3];
    for (int k = 0; k < 3; k++) l[k] = d[0] * v.axes[k * 3] + d[1] * v.axes[k * 3 + 1] + d[2] * v.axes[k * 3 + 2];
    if (l[1] < 0 || l[1] > v.scale[1]) return false;
    if (v.shape == DBG_SHAPE_CYLINDER) {
        float x = v.scale[0] > 0 ? l[0] / (0.5f * v.scale[0]) : 2, z = v.scale[2] > 0 ? l[2] / (0.5f * v.scale[2]) : 2;
        return x * x + z * z <= 1;
    }
    return fabsf(l[0]) <= 0.5f * v.scale[0] && fabsf(l[2]) <= 0.5f * v.scale[2];
}

static void draw_volumes(float scale) {
    enum { MAXV = 1024 };
    static DbgVolume vol[MAXV];
    ViewProj P;
    float aspect = 16.0f / 9.0f;
    if (!vk_dbgcam_vp(P.vp, &aspect)) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 0 || ds.y <= 0) return;
    if (aspect <= 0.1f || aspect > 10) aspect = 16.0f / 9.0f;
    if (ds.x / ds.y > aspect) P.h = ds.y, P.w = ds.y * aspect;
    else P.w = ds.x, P.h = ds.x / aspect;
    P.x0 = (ds.x - P.w) * 0.5f, P.y0 = (ds.y - P.h) * 0.5f;
    const char *st = cfg_value("NX2NAVI_DEBUG_VIEW_STYLE");
    int style = !strcmp(st, "solid") ? 2 : !strcmp(st, "both") ? 3 : 1;
    int opacity = cfg_int("NX2NAVI_DEBUG_VIEW_OPACITY");
    opacity = opacity < 10 ? 10 : opacity > 100 ? 100 : opacity;
    float range = (float)cfg_int("NX2NAVI_DEBUG_VIEW_RANGE");
    bool labels = cfg_bool("NX2NAVI_DEBUG_VIEW_LABELS") != 0;
    ImU32 line[DBG_VOL_KINDS], fill[DBG_VOL_KINDS];
    for (int k = 0; k < DBG_VOL_KINDS; k++) {
        int i = neon_index(cfg_value(kKindKey[k]));
        line[k] = IM_COL32(kNeon[i].r, kNeon[i].g, kNeon[i].b, 235);
        fill[k] = IM_COL32(kNeon[i].r, kNeon[i].g, kNeon[i].b, opacity * 255 / 100);
    }
    int n = dbg_view_volumes(vol, MAXV, range);
    float eye[3];
    bool have_eye = eye_of(P, eye);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float me[3];
    bool have_me = dbg_player_pos(me) != 0;
    // trigger zones first: the walls, loading zones and hidden things drawn after them stay on top
    static const int order[] = {DBG_VOL_TRIGGER, DBG_VOL_DEATH, DBG_VOL_LOAD, DBG_VOL_WALL, DBG_VOL_HIDDEN};
    static int idx[MAXV];
    int m = 0;
    for (int o : order)
        for (int i = 0; i < n; i++)
            if (vol[i].kind == o) idx[m++] = i;
    for (int t = 0; t < m; t++) {
        const DbgVolume &v = vol[idx[t]];
        int k = kind_slot(v.kind);
        // seen from inside, a filled volume would tint the whole picture: its edges only
        int vs = have_eye && (style & 2) && inside(v, eye) ? 1 : style;
        draw_volume(dl, P, v, vs, line[k], fill[k], 1.6f * scale);
        if (!labels) continue;
        float d2 = 0;
        if (have_me)
            for (int j = 0; j < 3; j++) d2 += (v.pos[j] - me[j]) * (v.pos[j] - me[j]);
        // The kill volume under the world is kilometres wide and far below: its name goes on its surface straight
        // under the player, with the height, at any distance.
        bool under = v.kind == DBG_VOL_DEATH && v.shape == DBG_SHAPE_BOX && v.scale[0] > 1000.0f && have_me;
        if (d2 > 60.0f * 60.0f && !under) continue;   // names only close by, or the far field fills with text
        float top[3], c[4];   // the name over the volume's top middle (an object's: 1 m over it)
        if (v.shape == DBG_SHAPE_MARKER) memcpy(top, v.pos, sizeof top), top[1] += 1.0f;
        else world_of(v, 0, 1, 0, top);
        if (under) top[0] = me[0], top[2] = me[2];
        clip_of(P, top, c);
        if (c[3] < kNear) continue;
        ImVec2 at = screen_of(P, c);
        char text_buf[112];   // a trigger says what it acts on
        if (v.kind != DBG_VOL_LOAD && v.target[0]) snprintf(text_buf, sizeof text_buf, "%s > %s", v.name, v.target);
        if (under) snprintf(text_buf, sizeof text_buf, "%s, y %.0f (%.0f m below)", v.name, top[1], me[1] - top[1]);
        const char *text = v.kind == DBG_VOL_LOAD ? "Loading zone" : (v.target[0] || under) ? text_buf : v.name;
        ImVec2 sz = ImGui::CalcTextSize(text);
        at.x -= sz.x * 0.5f, at.y -= sz.y;
        dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(at, line[k], text);
    }
}

extern "C" void debug_overlay_draw(float scale) {
    if (dbg_view_kinds() && dbg_available()) draw_volumes(scale);
    int mask = dbg_overlay_mask();
    const char *toast_msg = dbg_toast(plat_now_ns());
    bool igt = dbg_igt_enabled() != 0;
    if ((!mask && !toast_msg && !igt) || !dbg_available()) return;
    char lines[6][96];
    int n = 0;
    float pos[3];
    if (mask & DBG_SHOW_POS) {
        if (dbg_player_pos(pos))
            snprintf(lines[n++], sizeof lines[0], "X %.2f  Y %.2f  Z %.2f", pos[0], pos[1], pos[2]);
        else
            snprintf(lines[n++], sizeof lines[0], "no player");
    }
    if (mask & DBG_SHOW_AREA) {
        char cur[64] = "", banc[64], start[64];
        dbg_current_area(cur);
        int have = dbg_last_entrance(banc, start) && !strcmp(banc, cur);
        snprintf(lines[n++], sizeof lines[0], "%s%s%s", cur[0] ? cur : "no area", have ? "  from " : "", have ? start : "");
    }
    if (mask & DBG_SHOW_SPEED)
        snprintf(lines[n++], sizeof lines[0], "Speed %.2f m/s  vert %.2f", dbg_ground_speed(), dbg_vertical_speed());
    if (mask & DBG_SHOW_TIMER) {
        char t[32];
        format_time(t, sizeof t, dbg_timer_ns());
        snprintf(lines[n++], sizeof lines[0], "%s %s  %llu f", dbg_timer_running() ? ">" : "||", t,
                 (unsigned long long)dbg_timer_ticks());
        uint64_t laps[DBG_LAPS];
        int laps_n = dbg_timer_laps(laps);
        if (laps_n) {
            format_time(t, sizeof t, laps[laps_n - 1]);
            snprintf(lines[n++], sizeof lines[0], "Lap %d  %s", laps_n, t);
        }
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (igt) {
        // Speedrun panel, top right: IGT large, RTA, then the last few splits with their difference to the best run
        // (green ahead, red behind).
        uint64_t t_igt = 0, t_rta = 0;
        dbg_igt_times(&t_igt, &t_rta);
        char big[32], rta[48];
        format_time(big, sizeof big, t_igt);
        char tmp[32];
        format_time(tmp, sizeof tmp, t_rta);
        snprintf(rta, sizeof rta, "RTA %s%s", tmp, dbg_igt_running() ? "" : "  (stopped)");
        ImFont *font = ImGui::GetFont();
        float fs = ImGui::GetFontSize(), big_fs = fs * 1.8f, pad = 8.0f * scale, w = 480.0f * scale;
        int ns = dbg_igt_splits(), first = ns > 5 ? ns - 5 : 0;
        float h = pad * 2 + big_fs + fs * (1 + (ns - first)) + 4.0f * scale;
        // Below the game's item buttons at the top right.
        float x1 = ImGui::GetIO().DisplaySize.x - 14.0f * scale, x0 = x1 - w, y0 = ImGui::GetIO().DisplaySize.y * 0.24f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h), IM_COL32(0, 0, 0, 170), 6.0f * scale);
        ImVec2 bs = font->CalcTextSizeA(big_fs, 1e9f, 0.0f, big);
        ImU32 col = dbg_igt_running() ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 170, 170, 255);
        dl->AddText(font, big_fs, ImVec2(x1 - pad - bs.x, y0 + pad), col, big);
        float y = y0 + pad + big_fs + 2.0f * scale;
        ImVec2 rs = ImGui::CalcTextSize(rta);
        dl->AddText(ImVec2(x1 - pad - rs.x, y), IM_COL32(190, 190, 190, 255), rta);
        y += fs + 2.0f * scale;
        for (int i = first; i < ns; i++) {
            uint64_t sn;
            int64_t d;
            int hd;
            char name[48], st[32], dt[40];
            if (!dbg_igt_split_at(i, &sn, name, &d, &hd)) break;
            format_time(st, sizeof st, sn);
            char ad[32];
            format_time(ad, sizeof ad, (uint64_t)(d < 0 ? -d : d));
            snprintf(dt, sizeof dt, "%s%s", d < 0 ? "-" : "+", ad);
            ImVec2 ss = ImGui::CalcTextSize(st), ds = hd ? ImGui::CalcTextSize(dt) : ImVec2(0, 0);
            float name_end = x1 - pad - ss.x - 10.0f * scale - (hd ? ds.x + 10.0f * scale : 0.0f);
            dl->PushClipRect(ImVec2(x0, y), ImVec2(name_end, y + fs), true);
            dl->AddText(ImVec2(x0 + pad, y), IM_COL32(220, 220, 220, 255), name);
            dl->PopClipRect();
            dl->AddText(ImVec2(x1 - pad - ss.x, y), IM_COL32(255, 255, 255, 255), st);
            if (hd)
                dl->AddText(ImVec2(x1 - pad - ss.x - 10.0f * scale - ds.x, y),
                            d < 0 ? IM_COL32(90, 230, 120, 255) : IM_COL32(240, 90, 90, 255), dt);
            y += fs;
        }
    }
    if (toast_msg) {
        ImVec2 sz = ImGui::CalcTextSize(toast_msg);
        float pad2 = 8.0f * scale, cx = ImGui::GetIO().DisplaySize.x * 0.5f, y = ImGui::GetIO().DisplaySize.y * 0.12f;
        dl->AddRectFilled(ImVec2(cx - sz.x / 2 - pad2, y - pad2), ImVec2(cx + sz.x / 2 + pad2, y + sz.y + pad2), IM_COL32(0, 0, 0, 170), 4.0f * scale);
        dl->AddText(ImVec2(cx - sz.x / 2, y), IM_COL32(255, 255, 255, 255), toast_msg);
    }
    if (mask & DBG_SHOW_INPUT) {
        // Small, bottom left. Labels are drawn at their own font size so the panel can shrink without the text spilling out of the keys.
        const ImU32 body = IM_COL32(38, 40, 46, 215), edge = IM_COL32(110, 114, 124, 235), idle = IM_COL32(24, 25, 29, 255);
        const ImU32 lit = IM_COL32(255, 205, 50, 255), txt = IM_COL32(205, 208, 216, 255), txt_lit = IM_COL32(20, 20, 20, 255);
        ImFont *font = ImGui::GetFont();
        auto label = [&](float fs, ImVec2 c, const char *s, ImU32 col) {
            ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, s);
            dl->AddText(font, fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col, s);
        };
        if (input_kbm_last()) {
            // Keyboard and mouse were used last: the whole keyboard (keys the game has no id for are drawn but never light), the
            // navigation block, the numpad and the mouse, all inside one box.
            float ku = 14.0f * scale, fs = ku * 0.62f;
            float W = 29.4f * ku, H = 8.4f * ku, pad = 0.5f * ku;
            float ox = 14.0f * scale, oy = ImGui::GetIO().DisplaySize.y - 14.0f * scale - H;
            dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + W, oy + H), body, 0.8f * ku);
            dl->AddRect(ImVec2(ox, oy), ImVec2(ox + W, oy + H), edge, 0.8f * ku, 0, 1.2f * scale);
            struct K { int key; const char *n; float x, y, w; };
            std::vector<K> ks;
            auto add = [&](int key, const char *n, float x, float y, float w = 1.0f) { ks.push_back({key, n, x, y, w}); };
            add(0, "Esc", 0, 0);
            static const struct { int k; const char *n; } fk[] = {{0, "F1"}, {KEY_F2, "F2"}, {KEY_F3, "F3"}, {KEY_F4, "F4"}, {KEY_F5, "F5"}, {KEY_F6, "F6"}, {KEY_F7, "F7"},
                                                                  {0, "F8"}, {KEY_F9, "F9"}, {0, "F10"}, {0, "F11"}, {KEY_F12, "F12"}};
            for (int i = 0; i < 12; i++) add(fk[i].k, fk[i].n, 2.0f + i + (i / 4) * 0.5f, 0);
            static const char *nums = "1234567890";
            static char nl[10][2];
            add(KEY_GRAVE, "`", 0, 1.3f);
            for (int i = 0; i < 10; i++) { nl[i][0] = nums[i]; nl[i][1] = 0; add(i == 9 ? KEY_0 : KEY_1 + i, nl[i], 1.0f + i, 1.3f); }
            add(KEY_MINUS, "-", 11, 1.3f); add(KEY_EQUALS, "=", 12, 1.3f); add(KEY_BACKSPACE, "Bksp", 13, 1.3f, 2.0f);
            static const char *r1 = "QWERTYUIOP", *r2 = "ASDFGHJKL", *r3 = "ZXCVBNM";
            static char l1[10][2], l2[9][2], l3[7][2];
            add(KEY_TAB, "Tab", 0, 2.3f, 1.5f);
            for (int i = 0; i < 10; i++) { l1[i][0] = r1[i]; l1[i][1] = 0; add(KEY_A + (r1[i] - 'A'), l1[i], 1.5f + i, 2.3f); }
            add(KEY_LBRACKET, "[", 11.5f, 2.3f); add(KEY_RBRACKET, "]", 12.5f, 2.3f); add(KEY_BACKSLASH, "\\", 13.5f, 2.3f, 1.5f);
            add(KEY_CAPSLOCK, "Caps", 0, 3.3f, 1.75f);
            for (int i = 0; i < 9; i++) { l2[i][0] = r2[i]; l2[i][1] = 0; add(KEY_A + (r2[i] - 'A'), l2[i], 1.75f + i, 3.3f); }
            add(KEY_SEMICOLON, ";", 10.75f, 3.3f); add(KEY_APOSTROPHE, "'", 11.75f, 3.3f); add(KEY_ENTER, "Enter", 12.75f, 3.3f, 2.25f);
            add(KEY_LSHIFT, "Shift", 0, 4.3f, 2.25f);
            for (int i = 0; i < 7; i++) { l3[i][0] = r3[i]; l3[i][1] = 0; add(KEY_A + (r3[i] - 'A'), l3[i], 2.25f + i, 4.3f); }
            add(KEY_COMMA, ",", 9.25f, 4.3f); add(KEY_PERIOD, ".", 10.25f, 4.3f); add(KEY_SLASH, "/", 11.25f, 4.3f); add(KEY_RSHIFT, "Shift", 12.25f, 4.3f, 2.75f);
            add(KEY_LCTRL, "Ctrl", 0, 5.3f, 1.5f); add(0, "Win", 1.5f, 5.3f, 1.25f); add(KEY_LALT, "Alt", 2.75f, 5.3f, 1.25f);
            add(KEY_SPACE, "Space", 4.0f, 5.3f, 6.25f); add(0, "Alt", 10.25f, 5.3f, 1.25f); add(0, "Menu", 11.5f, 5.3f, 1.25f); add(KEY_RCTRL, "Ctrl", 12.75f, 5.3f, 2.25f);
            add(KEY_INSERT, "Ins", 15.5f, 1.3f); add(KEY_HOME, "Hm", 16.5f, 1.3f); add(KEY_PAGEUP, "PU", 17.5f, 1.3f);
            add(KEY_DELETE, "Del", 15.5f, 2.3f); add(KEY_END, "End", 16.5f, 2.3f); add(KEY_PAGEDOWN, "PD", 17.5f, 2.3f);
            add(KEY_UP, "^", 16.5f, 4.3f); add(KEY_LEFT, "<", 15.5f, 5.3f); add(KEY_DOWN, "v", 16.5f, 5.3f); add(KEY_RIGHT, ">", 17.5f, 5.3f);
            add(0, "Nm", 19.0f, 1.3f); add(KEY_NUM_DIV, "/", 20.0f, 1.3f); add(KEY_NUM_MUL, "*", 21.0f, 1.3f); add(KEY_NUM_SUB, "-", 22.0f, 1.3f);
            static const char *nn[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
            add(KEY_NUM7, nn[7], 19.0f, 2.3f); add(KEY_NUM8, nn[8], 20.0f, 2.3f); add(KEY_NUM9, nn[9], 21.0f, 2.3f); add(KEY_NUM_ADD, "+", 22.0f, 2.3f);
            add(KEY_NUM4, nn[4], 19.0f, 3.3f); add(KEY_NUM5, nn[5], 20.0f, 3.3f); add(KEY_NUM6, nn[6], 21.0f, 3.3f);
            add(KEY_NUM1, nn[1], 19.0f, 4.3f); add(KEY_NUM2, nn[2], 20.0f, 4.3f); add(KEY_NUM3, nn[3], 21.0f, 4.3f);
            add(KEY_NUM0, nn[0], 19.0f, 5.3f, 2.0f); add(KEY_NUM_DECIMAL, ".", 21.0f, 5.3f);
            for (auto &k : ks) {
                bool dn = k.key > 0 && input_key_held(k.key) != 0;
                ImVec2 a(ox + pad + k.x * ku, oy + pad + k.y * ku), b(a.x + k.w * ku - 0.12f * ku, a.y + 0.88f * ku);
                dl->AddRectFilled(a, b, dn ? lit : idle, 0.2f * ku);
                dl->AddRect(a, b, edge, 0.2f * ku, 0, 1.0f * scale);
                label(fs, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), k.n, dn ? txt_lit : txt);
            }
            {
                float mx = ox + pad + 23.6f * ku, my = oy + pad + 1.3f * ku;
                struct M { int key; const char *n; float x, w; } ms[] = {{KEY_MOUSE_L, "L", 0.0f, 1.7f}, {KEY_MOUSE_M, "M", 1.7f, 1.2f}, {KEY_MOUSE_R, "R", 2.9f, 1.7f}};
                dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + 4.6f * ku, my + 4.8f * ku), idle, 2.0f * ku);
                dl->AddRect(ImVec2(mx, my), ImVec2(mx + 4.6f * ku, my + 4.8f * ku), edge, 2.0f * ku, 0, 1.0f * scale);
                for (auto &m : ms) {
                    bool dn = input_key_held(m.key) != 0;
                    ImVec2 a(mx + m.x * ku, my), b(mx + (m.x + m.w) * ku, my + 1.9f * ku);
                    dl->AddRectFilled(a, b, dn ? lit : IM_COL32(48, 50, 58, 255), 0.5f * ku);
                    dl->AddRect(a, b, edge, 0.5f * ku, 0, 1.0f * scale);
                    label(fs, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), m.n, dn ? txt_lit : txt);
                }
                label(fs, ImVec2(mx + 2.3f * ku, my + 3.4f * ku), "mouse", txt);
            }
        } else {
            uint64_t btn = 0;
            int32_t lx = 0, ly = 0, rx = 0, ry = 0;
            input_poll_ui(&btn, &lx, &ly, &rx, &ry);
            // Switch npad bits: A B X Y, stick clicks, L R ZL ZR, plus minus, d-pad left up right down.
            // Drawn like a Pro Controller: shoulders on top, left stick and d-pad on the left, face buttons and right stick on the right.
            auto down = [&](int bit) { return ((btn >> bit) & 1) != 0; };
            float u = 8.5f * scale, fs = u * 0.95f;
            float W = 26.0f * u, H = 15.5f * u;
            float ox = 14.0f * scale, oy = ImGui::GetIO().DisplaySize.y - 14.0f * scale - H;
            auto P = [&](float x, float y) { return ImVec2(ox + x * u, oy + y * u); };
            struct Sh { int bit; const char *n; float x0, x1, y0, y1; } sh[] = {
                {8, "ZL", 1.5f, 8.5f, 0.0f, 1.7f}, {9, "ZR", 17.5f, 24.5f, 0.0f, 1.7f},
                {6, "L", 1.0f, 8.0f, 1.9f, 3.6f}, {7, "R", 18.0f, 25.0f, 1.9f, 3.6f},
            };
            for (auto &s : sh) {
                dl->AddRectFilled(P(s.x0, s.y0), P(s.x1, s.y1), down(s.bit) ? lit : edge, 0.8f * u);
                label(fs, P((s.x0 + s.x1) / 2, (s.y0 + s.y1) / 2), s.n, down(s.bit) ? txt_lit : IM_COL32(20, 20, 24, 255));
            }
            dl->AddRectFilled(P(0.0f, 3.4f), P(W / u, 13.2f), body, 3.0f * u);
            dl->AddCircleFilled(P(3.6f, 11.2f), 4.4f * u, body, 40);
            dl->AddCircleFilled(P(22.4f, 11.2f), 4.4f * u, body, 40);
            dl->AddRect(P(0.0f, 3.4f), P(W / u, 13.2f), edge, 3.0f * u, 0, 1.2f * scale);
            auto stick = [&](ImVec2 c, int32_t sx, int32_t sy, int clickbit) {
                float R = 2.5f * u;
                dl->AddCircleFilled(c, R, idle, 32);
                dl->AddCircle(c, R, edge, 32, 1.2f * scale);
                float tx = sx / 32767.0f, ty = -sy / 32767.0f, m = sqrtf(tx * tx + ty * ty);
                if (m > 1) { tx /= m; ty /= m; }
                ImVec2 k(c.x + tx * R * 0.55f, c.y + ty * R * 0.55f);
                dl->AddCircleFilled(k, R * 0.72f, down(clickbit) ? lit : IM_COL32(88, 92, 102, 255), 28);
                dl->AddCircle(k, R * 0.72f, edge, 28, 1.0f * scale);
            };
            stick(P(5.4f, 6.2f), lx, ly, 4);
            stick(P(16.6f, 10.6f), rx, ry, 5);
            {
                ImVec2 c = P(9.4f, 10.6f);
                struct D { int bit; float dx, dy; const char *n; } d[] = {{13, 0, -1, "^"}, {15, 0, 1, "v"}, {12, -1, 0, "<"}, {14, 1, 0, ">"}};
                float a = 1.15f * u;
                for (auto &e : d) {
                    ImVec2 m(c.x + e.dx * 1.55f * u, c.y + e.dy * 1.55f * u);
                    dl->AddRectFilled(ImVec2(m.x - a, m.y - a), ImVec2(m.x + a, m.y + a), down(e.bit) ? lit : idle, 0.5f * u);
                    dl->AddRect(ImVec2(m.x - a, m.y - a), ImVec2(m.x + a, m.y + a), edge, 0.5f * u, 0, 1.0f * scale);
                    label(fs, m, e.n, down(e.bit) ? txt_lit : txt);
                }
                dl->AddRectFilled(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), idle);
            }
            {
                ImVec2 c = P(21.0f, 6.2f);
                struct F { int bit; float dx, dy; const char *n; } f[] = {{2, 0, -1, "X"}, {1, 0, 1, "B"}, {3, -1, 0, "Y"}, {0, 1, 0, "A"}};
                for (auto &e : f) {
                    ImVec2 m(c.x + e.dx * 2.0f * u, c.y + e.dy * 2.0f * u);
                    dl->AddCircleFilled(m, 1.35f * u, down(e.bit) ? lit : idle, 24);
                    dl->AddCircle(m, 1.35f * u, edge, 24, 1.0f * scale);
                    label(fs, m, e.n, down(e.bit) ? txt_lit : txt);
                }
            }
            const struct { int bit; const char *n; float x; } mp[] = {{11, "-", 10.6f}, {10, "+", 15.4f}};
            for (auto &e : mp) {
                ImVec2 m = P(e.x, 4.7f);
                dl->AddCircleFilled(m, 0.9f * u, down(e.bit) ? lit : idle, 20);
                dl->AddCircle(m, 0.9f * u, edge, 20, 1.0f * scale);
                label(fs, m, e.n, down(e.bit) ? txt_lit : txt);
            }
        }
    }
    if (!n) return;
    float pad = 6.0f * scale, line_h = ImGui::GetTextLineHeight();
    float w = 0;
    for (int i = 0; i < n; i++) w = fmaxf(w, ImGui::CalcTextSize(lines[i]).x);
    ImVec2 a(10.0f * scale, 10.0f * scale), b(a.x + w + 2 * pad, a.y + n * line_h + 2 * pad);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), 4.0f * scale);
    for (int i = 0; i < n; i++) dl->AddText(ImVec2(a.x + pad, a.y + pad + i * line_h), IM_COL32(255, 255, 255, 255), lines[i]);
}
