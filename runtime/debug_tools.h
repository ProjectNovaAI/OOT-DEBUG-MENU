// Debug tools of the in-game menu's Debug tab (debug_tools.c, ui/debug_tab.cpp): the player's position and speed, saved
// positions to return to, a game-speed scale, a timer with laps and the on-screen readout.
//
// Only game build 1.1.1 is supported (host.h RT_V111): its anchors into the guest's memory are the ones frame_rate.c
// already uses. On any other build dbg_available() is 0, every reader fails and every writer does nothing.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DBG_SLOTS = 4, DBG_LAPS = 8 };

// The debug tools work on this game build (the memory anchors are known).
int dbg_available(void);

// Player actor. pos is metres, the world frame the game uses. 1 on success, 0 while no save is loaded (the actor does
// not exist yet) or on an unsupported build.
int dbg_player_pos(float pos[3]);
// Moves the player actor to pos (the position fields only: velocity and the physics body are left to the game, which
// may pull the actor back). 1 when written.
int dbg_player_set_pos(const float pos[3]);

// Once per presented frame (the overlay path): follows the position to estimate the speed.
void dbg_tick(uint64_t now_ns);
// Speed over the last few presents in metres per real second: along the ground (x and z) and vertical (y, up positive).
// Both 0 until two positions have been seen. A jump of more than 20 m in one present (a teleport, a loading screen)
// is not counted as speed.
float dbg_ground_speed(void);
float dbg_vertical_speed(void);

// Saved positions ("save states" of the place only, not of the game's state).
int dbg_slot_save(int slot);       // 1 when the player was found and the position kept
int dbg_slot_load(int slot);       // 1 when written
int dbg_slot_used(int slot, float pos[3]);

// Game speed: a factor applied to the step the game is given each frame (frame_rate.c ocarina_step_write). 1 is normal.
// The step is kept within what the game's physics survives, so the lowest effective factor depends on the frame rate
// (about 1/8 at 60 fps) and the highest about 4. Needs the free-running step of the Ocarina build (NX2NAVI_OCARINA_STEP
// not 0).
float dbg_time_scale(void);
void dbg_set_time_scale(float scale);

// Timer: real time since start, in nanoseconds, with laps; and the game's own 60 Hz ticks over the same span.
void dbg_timer_toggle(void);       // start or stop
void dbg_timer_reset(void);        // zero, stopped, laps cleared
void dbg_timer_lap(void);          // keeps the current time as the next lap
int dbg_timer_running(void);
uint64_t dbg_timer_ns(void);
uint64_t dbg_timer_ticks(void);
int dbg_timer_laps(uint64_t ns[DBG_LAPS]);   // the number of laps, their times in order

// On-screen readout lines (bit mask).
enum { DBG_SHOW_POS = 1, DBG_SHOW_SPEED = 2, DBG_SHOW_TIMER = 4 };
int dbg_overlay_mask(void);
void dbg_overlay_set(int mask);

#ifdef __cplusplus
}
#endif
