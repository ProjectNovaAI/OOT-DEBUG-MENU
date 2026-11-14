// The Randomizer tab of the in-game menu: the generator's options (randomizer.c, nx2navi.ini [randomizer]), the seed,
// and Generate, which writes the Randomizer mod in the background; the new seed applies at the next start.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "randomizer.h"
void menu_ask_restart(void);   // ui/menu.cpp: the Restart confirmation
}

// What each option's values are called and do, in the order of rando_options (randomizer.c).
static const struct {
    const char *label;
    const char *names[6];
    const char *desc;
} kOptions[] = {
    {"Logic", {"Glitchless", "No logic"},
     "Glitchless: every item can be reached without tricks, Ganon's Castle included. No logic: anything anywhere; an "
     "item can end up behind itself (the Debug tab can give it). Overworld and All entrances turn the logic off."},
    {"Chests", {"Shuffled", "Vanilla"},
     "What the treasure chests hold. Small keys and boss keys stay in their own chests; the Treasure Chest Shop and "
     "the Deku and Hylian Shield chests stay as they are."},
    {"Pots, grass and drops", {"Vanilla", "Among themselves", "With the chests"},
     "What pots, grass, rocks, bushes, trees, crates, beehives and enemies with a set drop give. Among themselves: "
     "their items trade places. With the chests: one pool with the chests, so a pot can hold a chest's item. Only "
     "items the game can drop go to them; equipment stays in chests."},
    {"Maps and compasses", {"Own dungeon", "Vanilla"},
     "Own dungeon: each dungeon's map and compass move between that dungeon's chests. Vanilla: they stay."},
    {"Enemies", {"Vanilla", "Same kind", "Any kind"},
     "Lone enemies become other enemies. Same kind: ground for ground, flying for flying, water for water. Any kind: "
     "any ground or flying enemy. Enemies a door, a fight or an event waits on stay."},
    {"Entrances", {"Vanilla", "Dungeons", "Boss rooms", "Overworld", "All"},
     "Reconnects loading zones in pairs, so the way back leads back. Dungeons: dungeon entrances. Boss rooms: the "
     "boss rooms that are stages of their own. Overworld: the field, town and village connections. All: all of them. "
     "Ganon's Castle stays where it is."},
    {"Entrance pools", {"Separate", "Mixed"},
     "Separate: dungeons lead only to other dungeons (child to child, adult to adult), boss rooms only to boss rooms "
     "and field exits only to field exits. Mixed: the shuffled entrances share one pool with the field and town "
     "connections, so a dungeon can be behind any door and a dungeon's door can lead anywhere. Mixed turns the logic "
     "off."},
    {"Dungeon ages", {"Same age", "Any age"},
     "Same age: a child dungeon's door leads to a child dungeon and an adult dungeon's to an adult dungeon. Any age: "
     "a dungeon door can lead to a dungeon of either age, so the Deku Tree can hold the Forest Temple. With Mixed "
     "pools ages are mixed anyway."},
};

extern "C" void tab_randomizer(float scale) {
    rando_tick();
    const bool running = rando_state() == RANDO_RUNNING;
    opt_begin("##randomizer", scale);

    opt_group_text("Seed");
    {
        const char *seed = rando_seed();
        if (running) opt_disable_next_text("Generating...");
        int r = opt_text_row("##rando_seed", "Seed", *seed ? seed : "new", nullptr,
                             "The same seed with the same options gives the same game, so it can be shared. Press A "
                             "for a new seed.",
                             false);
        if (r && !running) {
            rando_new_seed();
            opt_save_soon();
        }
    }
    if (running) opt_disable_next_text("Generating...");
    if (opt_text_row("##rando_generate", "Generate", running ? "Working..." : "Press A", nullptr,
                     "Makes the Randomizer mod with this seed and these options. Takes about a minute; the game keeps "
                     "running meanwhile.",
                     false) == 2 &&
        !running)
        rando_generate();
    if (*rando_note()) opt_text_str(rando_note(), rando_state() == RANDO_FAILED);
    if (rando_state() == RANDO_DONE &&
        opt_text_row("##rando_restart", "Restart now", "Press A", nullptr, "Closes the game and starts it again with "
                     "the new seed. Save first: the game restarts where its save left off.", false) == 2)
        menu_ask_restart();
    {
        char sp[PLAT_PATH_MAX];
        if (rando_spoiler_path(sp, sizeof sp) &&
            opt_text_row("##rando_spoiler_open", "Spoiler log", "Open folder", nullptr, "Opens the Randomizer mod's "
                         "folder, which holds spoiler.txt: where every item went, and the entrances and enemies that "
                         "changed.", false) == 2) {
            char *cut = strrchr(sp, '\\');
            if (!cut) cut = strrchr(sp, '/');
            if (cut) *cut = 0;
            plat_open_folder(sp);
        }
    }

    opt_group_text("Items");
    for (int i = 0; i < rando_noptions; i++) {
        if (i == 4) opt_group_text("World");
        const RandoOption *o = &rando_options[i];
        if (running) opt_disable_next_text("Generating...");
        char id[48];
        snprintf(id, sizeof id, "##%s", o->key);
        const int entrances = rando_noptions - 3;   // Entrances, then Entrance pools and Dungeon ages
        if (i > entrances && rando_option_index(entrances) == 0) opt_disable_next_text("Turn Entrances on to use it.");
        else if (i == entrances + 2 && rando_option_index(entrances + 1) == 1)
            opt_disable_next_text("Mixed pools mix the ages already.");
        else if (i == entrances + 2 && rando_option_index(entrances) != 1 && rando_option_index(entrances) != 4)
            opt_disable_next_text("Needs Entrances on Dungeons or All.");
        int r = opt_text_choice(id, kOptions[i].label, kOptions[i].names, o->nvalues, rando_option_index(i),
                                kOptions[i].desc);
        if (r >= 0 && !running) opt_apply(o->key, o->values[r]);
    }
    {
        const char *sp = cfg_get("NX2NAVI_RANDO_SPOILER");
        bool on = !sp || strcmp(sp, "0");
        if (running) opt_disable_next_text("Generating...");
        if (opt_toggle_text("##rando_spoiler", "Write a spoiler log", "spoiler.txt in the Randomizer mod's folder "
                            "lists where everything went.", on) && !running)
            opt_apply("NX2NAVI_RANDO_SPOILER", on ? "0" : "1");
    }

    opt_group_text("About");
    opt_text_str("Generate writes the Randomizer mod from your own game files and puts it at the top of the Mods "
                 "order; it applies when the game starts again. Start a new save file for a new seed: the chests an "
                 "old save opened stay open. Turn the mod off on the Mods tab to play the normal game again.");
    opt_text_str("Not shuffled yet: songs, gifts from people, Gold Skulltula rewards, shops and Deku Scrubs. Pots, grass "
                 "and drops give their item each time they are broken, as in the normal game.");
    opt_end();
}
