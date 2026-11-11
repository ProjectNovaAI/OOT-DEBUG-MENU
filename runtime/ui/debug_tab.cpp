// The Debug tab of the in-game menu and the on-screen readout it switches (debug_tools.h): the player's position and
// speed, saved positions to teleport between, the game speed, a timer with laps. The tools need game build 1.1.1;
// elsewhere the tab says so and offers nothing. The texts are English only, like the mods' cheat names: this is a
// tool for testing, not part of the game's menus.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "debug_remote.h"
#include "debug_scan.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
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
        };
        for (auto &r : rows)
            if (opt_toggle_text(r.id, r.label, r.desc, (mask & r.bit) != 0)) mask ^= r.bit;
        dbg_overlay_set(mask);
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

    opt_group_text("Not built yet");
    opt_text_str("Warping to any area, adding and removing items, switching between child and adult, flags, and showing "
                 "death barriers and invisible walls (also as a solid colour) need the game's own save and scene functions "
                 "found first. Notes on how far that got: docs/debug-menu.md.");
    opt_end();
}

// ---- the readout, top left, over the game

extern "C" void debug_overlay_draw(float scale) {
    int mask = dbg_overlay_mask();
    if (!mask || !dbg_available()) return;
    char lines[4][96];
    int n = 0;
    float pos[3];
    if (mask & DBG_SHOW_POS) {
        if (dbg_player_pos(pos))
            snprintf(lines[n++], sizeof lines[0], "X %.2f  Y %.2f  Z %.2f", pos[0], pos[1], pos[2]);
        else
            snprintf(lines[n++], sizeof lines[0], "no player");
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
    if (!n) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float pad = 6.0f * scale, line_h = ImGui::GetTextLineHeight();
    float w = 0;
    for (int i = 0; i < n; i++) w = fmaxf(w, ImGui::CalcTextSize(lines[i]).x);
    ImVec2 a(10.0f * scale, 10.0f * scale), b(a.x + w + 2 * pad, a.y + n * line_h + 2 * pad);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), 4.0f * scale);
    for (int i = 0; i < n; i++) dl->AddText(ImVec2(a.x + pad, a.y + pad + i * line_h), IM_COL32(255, 255, 255, 255), lines[i]);
}
