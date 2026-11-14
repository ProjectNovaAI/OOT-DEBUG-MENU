// The Mods tab of the in-game menu: Play as Lank, then a switch for each mod folder (mods.c) in the Mods order, with the
// mod's setting (mods.h) on a row under it; a setting several mods share is one row, under the first of them. A mod's
// switch and setting apply at the next start; the row says so while they differ from what this run started with.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "debug_tools.h"
#include "mods.h"
}

// What the mods made for this build do, by folder name; another mod shows its folder's state only.
static const char *mod_desc(const char *name) {
    static const struct { const char *name, *desc; } kKnown[] = {
        {"Fishing on horse", "The fishing rod can be used while riding, as the bow can."},
        {"Lake Hylia fishing", "The fishing pond owner also stands on the lake island, so the rod can be had "
                               "outside the pond. Adds bass and loaches that bite the lure and fish to catch in a "
                               "bottle to the lake, and takes the enemies off the lake and its shores, as child and "
                               "adult. Fish count sets how many fish."},
    };
    for (const auto &k : kKnown)
        if (!strcmp(k.name, name)) return k.desc;
    return "A mod from the mods folder. Changes apply at the next start.";
}

extern "C" void tab_mods(float scale) {
    opt_begin("##mods", scale);
    opt_group_text("Misc");
    if (!dbg_available()) opt_disable_next_text("Needs game version 1.1.1.");
    bool lank = dbg_lank_on() != 0;
    if (opt_toggle_text("##mod_lank", "Play as Lank", "Gives child Link the adult skeleton's proportions and adult Link "
                        "the child's. Keeps the current model, animations and gameplay age.", lank))
        dbg_lank(!lank);
    if (!dbg_available()) opt_disable_next_text("Needs game version 1.1.1.");
    bool bunny = dbg_bunny_on() != 0;
    if (opt_toggle_text("##mod_bunny", "Bunny Hood", "While the Bunny Hood is worn, Link runs faster and jumps further. "
                        "Bunny speed sets how much.", bunny))
        dbg_bunny(!bunny);
    {
        static const char *const names[] = {"1.25x", "1.5x", "1.75x", "2x", "2.5x", "3x"};
        static const float vals[] = {1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
        int cur = 1;
        float f = dbg_bunny_speed();
        for (int k = 0; k < 6; k++)
            if (f > vals[k] - 0.01f && f < vals[k] + 0.01f) cur = k;
        if (!bunny) opt_disable_next_text("Turn Bunny Hood on to use it.");
        int r = opt_text_choice("##mod_bunny_speed", "Bunny speed", names, 6, cur,
                                "How much further Link gets with the Bunny Hood on: running speed and jump distance.");
        if (r >= 0) dbg_set_bunny_speed(vals[r]);
    }
    if (!dbg_available()) opt_disable_next_text("Needs game version 1.1.1.");
    bool powers = dbg_mask_powers_on() != 0;
    if (opt_toggle_text("##mod_mask_powers", "Mask abilities", "While the Zora Mask is worn, Link swims about three times "
                        "faster and dives faster; while the Goron Mask is worn, he rolls twice as fast.", powers))
        dbg_mask_powers(!powers);
    for (int i = 0; i < mods_count(); i++) {
        ModInfo *m = mods_at(i);
        if (!m) continue;
        char id[300], sid[300];
        snprintf(id, sizeof id, "##mod_%d", i);
        snprintf(sid, sizeof sid, "##mod_set_%d", i);
        bool on = m->enabled != 0;
        const char *value = on != (m->start_enabled != 0) ? (on ? "On after restart" : "Off after restart") : nullptr;
        if (m->problem != MOD_OK) opt_disable_next_text("This mod cannot be used: see the log.");
        if (opt_toggle_text(id, m->name, mod_desc(m->name), on, value)) {
            mods_set_enabled(i, !on);
            opt_save_soon();
        }
        // a setting several mods share is one row, under the first of them; it applies while any of them is on
        bool first = true, any_on = false;
        for (int j = 0; j < mods_count() && m->nvalues; j++) {
            const ModInfo *o = mods_at(j);
            if (!o || !o->nvalues || strcmp(o->setting, m->setting)) continue;
            if (j < i) first = false;
            any_on |= o->enabled != 0;
        }
        if (m->nvalues && first) {
            const char *names[MOD_VALUES];
            for (int k = 0; k < m->nvalues; k++) names[k] = m->values[k];
            char shown[96];
            const char *sv = nullptr;
            if (m->value != m->start_value) {
                snprintf(shown, sizeof shown, "%s after restart", m->values[m->value]);
                sv = shown;
            }
            if (!any_on) opt_disable_next_text("Turn a mod that uses this setting on to use it.");
            int r = opt_text_choice(sid, m->setting, names, m->nvalues, m->value,
                                    "Used by every mod with a setting of this name. Applies at the next start.", false, sv);
            if (r >= 0 && r != m->value) {
                mods_set_value(i, r);
                opt_save_soon();
            }
        }
    }
    opt_end();
}
