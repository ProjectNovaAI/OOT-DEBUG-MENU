// Save profiles and manual saves (save_slots.c), for the menu's Game tab. Kept in <game folder>/saves/profiles:
//
//   active.txt                   the profile whose progress is the game's save now ("Default" when missing)
//   <profile>/                   one folder per profile; a new one is "Profile <n>"
//   <profile>/progress.zip       that profile's save while another profile is played (none: it starts a new game)
//   <profile>/manual/<name>.zip  its manual saves, any number: "<date> <time> <area>"
//
// The game's own save is one folder that the game reads at its start and writes as it plays, so a switch and a load
// take effect at the next start: the save to load is staged as an import (save_share.h, fs_host.c import_pending), which
// also backs up the save it replaces, and the menu offers the restart. A manual save and a switch take the save the game
// last wrote (its autosave, or a save from its System page), not progress made since. Calls do file work: menu worker
// thread, one at a time.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SAVE_NAME_MAX = 96, SAVE_LIST_MAX = 256 };

// The profile in use, after taking the result of a switch the last start made (fs_save_import_tag_take). 0 when the
// saves folder is unknown.
int save_profile_active(char out[SAVE_NAME_MAX]);
// The profiles, sorted, the one in use among them. The number listed.
int save_profile_list(char names[][SAVE_NAME_MAX], int max);
// A new profile "Profile <n>" (n the first free number from 2): its name in out. 0 on success.
int save_profile_create(char out[SAVE_NAME_MAX]);
// Puts the game's save into the profile in use (progress.zip) and stages the save of `name` (its progress.zip, or no
// save) for the next start. 0 on success; the profile in use changes once that start made it the save.
int save_profile_switch(const char *name);
// The same switch, made at once (fs_save_import_now): only where the game holds nothing of its save, its title screen
// before a slot is loaded. 0: the save of name is the save now; 1: the game must restart to finish (the working copy
// was in use, or one of the two profiles has no save: the title shows Continue only with a save, from its start, so
// that switch is staged for the next start); -1: nothing changed.
int save_profile_switch_now(const char *name);
// Keep the running session and make name its save destination. Archives both profiles before changing active.
int save_profile_adopt(const char *name);
// 1 when the profile has a save: the one in use a save slot in the game's save, another one a saved slot inside its progress.zip.
int save_profile_has_save(const char *name);
// Deletes a profile that is not in use, with its manual saves. 0 on success.
int save_profile_delete(const char *name);

// The manual saves of the profile in use, newest first (by name: the names start with the date). The number listed.
int save_manual_list(char names[][SAVE_NAME_MAX], int max);
// A manual save of the game's save now, named "<date> <time> <area>": its name in out. 0 on success, 1 when the game
// has not saved yet, -1 on a failure.
int save_manual_make(char out[SAVE_NAME_MAX]);
// Stages that manual save for the next start. 0 on success.
int save_manual_load(const char *name);
int save_manual_delete(const char *name);

// The profile picker on the game's title screen (save_title.c): L / R switch to the next or previous profile at once,
// and the game's Load list shows its saves (save_profile_switch_now, then dbg_save_list_reload); to or from a profile
// with no save, A confirms and the game restarts. input.c poll_merged
// hands it the game's buttons (it takes L and R there, and A and B while a switch runs); the overlay
// (ui/debug_tab.cpp) draws what save_title_view returns.
enum {
    SAVE_TITLE_IDLE,         // the profile in use is shown
    SAVE_TITLE_CHOSEN,       // a profile with no save (or from one): A switches with a restart, B stays
    SAVE_TITLE_SWITCHING,    // save_profile_switch_now is running
    SAVE_TITLE_RELOADING,    // the game reads its save list again
    SAVE_TITLE_RESTART,      // the switch needs a restart: the render thread restarts the game (save_title_restart_due)
    SAVE_TITLE_RESTARTING,
    SAVE_TITLE_FAILED,       // the switch failed; the profile in use stays
};
typedef struct {
    char name[SAVE_NAME_MAX];     // the profile chosen (the one being switched to while a switch runs)
    char active[SAVE_NAME_MAX];   // the profile in use
    int index, count, phase;
    int saving;                  // Save screen: choose where the current session saves
} SaveTitleView;
void save_title_input(uint64_t *buttons);
int save_title_view(SaveTitleView *v);   // 1 while the picker shows
int save_title_showing(void);
int save_title_restart_due(void);        // 1 once: the switch needs a restart and the game is not writing its save

#ifdef __cplusplus
}
#endif
