// The Randomizer tab's work (randomizer.c): runs tools/randomizer.py, which writes the Randomizer mod (a RomFS mod in
// the mods folder, mods.h) from the player's own game files with the tab's options, and puts that mod first in the
// Mods order. The options are nx2navi.ini keys of the [randomizer] section (NX2NAVI_RANDO_*, cfg.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define RANDO_MOD "Randomizer"

// The generator's options: an nx2navi.ini key, the tool's argument and its values; values[0] is the default.
typedef struct {
    const char *key, *arg;
    const char *values[6];
    int nvalues;
} RandoOption;
extern const RandoOption rando_options[];
extern const int rando_noptions;
int rando_option_index(int i);     // the value in effect of option i, an index into its values

// mods_scan, before it lists the mods folder: a seed made last session (mods/Randomizer/romfs.new) into place.
void rando_apply_pending(const char *mods_dir);

enum { RANDO_IDLE, RANDO_RUNNING, RANDO_DONE, RANDO_FAILED };
int rando_state(void);             // RANDO_*; the result of the last run of this session
const char *rando_note(void);      // what the last run said, or why it could not start
int rando_generate(void);          // starts a run with the current options; 0 when it could not start (rando_note)
void rando_new_seed(void);         // a new random seed into NX2NAVI_RANDO_SEED
const char *rando_seed(void);      // the seed the next run uses ("" none yet)
int rando_mod_present(void);       // the mods folder holds a Randomizer mod
int rando_spoiler_path(char *out, int cap);   // the spoiler log's path, 1 when it exists
void rando_tick(void);             // once a frame while the tab shows: picks up the run's result

#ifdef __cplusplus
}
#endif
