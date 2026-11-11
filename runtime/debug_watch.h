// Change monitor of the Debug tab (debug_watch.c): while it is on, every flag of the game's flag stores (bool, int,
// float, string, vector and enum) and the list of loaded actors are compared with what they were, and each change is
// logged with the time since the monitor started: "12.40 int PouchData.Item_Bomb.StockNum 20 -> 19",
// "13.02 actor +1 Enemy_Skulltula (now 2)". For glitch hunting: do one thing in game and see what it touched. The
// lines are also written to debug_changes.txt next to the game.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// On: takes a fresh baseline (and forgets the ignored flags), then logs changes. Off: stops.
void dbg_watch_set(int on);
int dbg_watch_on(void);
// Flags that have changed since the baseline or the last Clear are ignored from now on: do nothing in game for a few
// seconds, then this, to keep out the timers and counters that always change.
void dbg_watch_ignore_changed(void);
// Empties the list and takes a new baseline; the ignored flags stay ignored.
void dbg_watch_clear(void);
int dbg_watch_count(void);                          // lines kept, up to 256
int dbg_watch_line(int i, char *out, size_t n);     // line i, 0 the newest; 0 when out of range
int dbg_watch_ignored(void);                        // flags ignored, by hand or for changing all the time
int dbg_watch_names(void);                          // flag names known (debug_flag_names.json next to the game)
const char *dbg_watch_state(void);                  // what it is doing, for the menu

#ifdef __cplusplus
}
#endif
