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
// Moves the player actor to pos. With the debug frame hook running this is dbg_player_teleport; without it only the
// position fields are written (the physics body is left to the game, which may pull the actor back). 1 when done.
int dbg_player_set_pos(const float pos[3]);
// The game's own warp of the player to pos at the next frame (debug_warp.c): the physics body moves too. 0 when the
// debug frame hook is not running (patch set dbgframe) or there is no player.
int dbg_player_teleport(const float pos[3]);

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
enum { DBG_SHOW_POS = 1, DBG_SHOW_SPEED = 2, DBG_SHOW_TIMER = 4, DBG_SHOW_INPUT = 8 };
int dbg_overlay_mask(void);
void dbg_overlay_set(int mask);

// Hotkeys on the keyboard while the menu is closed (off by default; the Debug tab switches them): F5 saves the position in
// slot 1, F6 goes to it, F7 and F8 lower and raise the game speed, F9 starts and stops the timer. dbg_hotkey is called with
// the function key's number by the window code; it returns 1 when it used the key. dbg_toast is the last message to show.
int dbg_hotkeys(void);
void dbg_hotkeys_set(int on);
int dbg_hotkey(int fkey);
const char *dbg_toast(uint64_t now_ns);   // NULL when there is none to show

// Health, in quarter hearts. The game keeps it as flag entries in the heap: 16 bytes {value, key hash, pointer into the game's
// code}, the health flag (key 0x31AB5580) followed by the maximum (key 0xFBE01DA1). The entry moves between runs, so a helper
// thread finds it once by its key (about half a minute) and the address is re-checked on every use.
// dbg_health_get: 1 and the values when the entry is known and valid; 0 while it is being looked for (the first call starts the
// search) or before a save is loaded.
int dbg_health_get(int *cur, int *max);
int dbg_health_locating(void);
int dbg_health_set(int cur);          // clamps to 0..max; 1 when written
int dbg_health_set_max(int max);      // quarter hearts, 4..80
// Invulnerable: health is raised back to the maximum on every presented frame (a single hit that kills in the same frame is
// not caught).
// Lock: health is held at exactly this many quarter hearts (capped at the maximum) on every presented frame, so it is the same
// after any damage or healing. 0 switches the lock off. With both a lock and the invulnerable switch on, the lock wins.
void dbg_health_lock(int quarters);
int dbg_health_lock_value(void);
// Int flags (rupees, item counts, small keys ...). The game keeps every int flag of its flag list in one heap array of
// 0x28-byte entries, in list order: a pointer to one class (main+0x4c20718), then the value at +0x1c and again at +0x20.
// A table of {key, index} pairs right before the array maps a flag's key (dbg_flag_key of its name) to its entry. A helper
// thread finds the array once (about half a minute); get and set return 0 until then, or when the key is unknown.
// Health and magic are the exception: the player keeps its own copy and writes it over the flag on every frame.
uint32_t dbg_flag_key(const char *name);
int dbg_flag_get(uint32_t key, int *value);
int dbg_flag_set(uint32_t key, int value);
int dbg_flags_locating(void);
// The other flag stores work the same way, each its own array with its own entry class and table; one search finds them
// all (the int store first, then the run of each other type nearest to it). Strings are the 64-byte kind; an enum's value
// is the hash of its value's name.
enum { DBG_FLAG_BOOL, DBG_FLAG_INT, DBG_FLAG_FLOAT, DBG_FLAG_STRING, DBG_FLAG_VEC3, DBG_FLAG_ENUM, DBG_FLAG_TYPES };
uint64_t dbg_flag_addr(int type, uint32_t key);   // the value's address, 0 when unknown (or still being looked for)
int dbg_flag_type(uint32_t key);                  // the store that has this key, -1 if none
int dbg_flag_get_bool(uint32_t key);              // 0 or 1, -1 when unknown
int dbg_flag_set_bool(uint32_t key, int on);
int dbg_flag_get_u32(int type, uint32_t key, uint32_t *value);   // int, float (bits) and enum stores
int dbg_flag_set_u32(int type, uint32_t key, uint32_t value);
int dbg_flag_get_string(uint32_t key, char out[64]);
int dbg_flag_set_string(uint32_t key, const char *text);
int dbg_flag_get_vec3(uint32_t key, float v[3]);
int dbg_flag_set_vec3(uint32_t key, const float v[3]);
// Magic, in the game's units (the maximum is 0 before the meter is obtained). Infinite: refilled on every presented frame.
int dbg_magic_get(int *cur, int *max);
int dbg_magic_set(int value);         // clamps to 0..max; 1 when written
void dbg_magic_infinite(int on);
int dbg_magic_infinite_on(void);
// The counted things of the menu (rupees, ammunition, small keys): label, largest value and step of entry i (NULL past the
// end), and its value. dbg_item_set clamps to 0..max.
int dbg_item_count(void);
const char *dbg_item_label(int i, int *max, int *step);
int dbg_item_get(int i, int *value);
int dbg_item_set(int i, int value);
// Infinite items: rupees and the ammunition (arrows, bombs, bombchus, sticks, nuts, seeds, beans) are set back to their
// largest value on every presented frame. Gold tokens and small keys are left alone.
void dbg_items_infinite(int on);
int dbg_items_infinite_on(void);
void dbg_invulnerable(int on);
int dbg_invulnerable_on(void);
uint64_t dbg_player_actor(void);   // the player actor's address, 0 when there is none
// Owned things (equipment, items, upgrades, quest items, songs, masks): label and menu group of entry i (NULL past the
// end); get is 1 owned, 0 not, -1 unknown (flags not found yet); set gives or removes.
int dbg_owned_count(void);
const char *dbg_owned_label(int i, const char **group);
int dbg_owned_get(int i);
int dbg_owned_set(int i, int on);
// Magic meter level: 0 none, 1 single, 2 double (-1 unknown).
int dbg_magic_level(void);
int dbg_magic_set_level(int level);

// Warps (debug_warp.c). Destination i is an area file, an entrance actor in it and the arrival event. A warp is armed:
// the next area change the game makes (a loading zone, a door, a warp song, an instant warp) goes to the destination
// instead. Areas with a child and an adult file have their age in the name; the area sets the age.
enum { DBG_AGE_ANY, DBG_AGE_CHILD, DBG_AGE_ADULT };
int dbg_warp_count(void);
const char *dbg_warp_banc(int i);
const char *dbg_warp_start(int i);
int dbg_warp_age(int i);
int dbg_warp_other_age(int i);         // the same entrance in the other age's file, -1 if none
int dbg_warp_arm(int i);               // 1 when armed (0 while the flags are still being looked for)
void dbg_warp_cancel(void);
// With a warp armed: puts the player into the nearest loading zone of the area that starts a change (a helper thread
// tries them in turn, the note says how it went). 1 when started.
int dbg_warp_now(void);
int dbg_warp_armed(void);              // the armed destination, -1 if none
// Age on arrival for the warps: DBG_AGE_ANY lets the area choose (its file's age), child or adult is held over the
// area's choice while it loads, so also in areas made for the other age.
void dbg_warp_set_age(int age);
int dbg_warp_get_age(void);
// Child and adult switch: an instant warp into this area's file for the other age, at the entrance the player last came
// in by when that file has it. dbg_age_switch_target is that warp, -1 when the area has one age only.
int dbg_age_switch_target(void);
int dbg_age_switch(void);
const char *dbg_warp_note(void);       // what the warp did last, for the menu
int dbg_current_area(char out[64]);    // Sequence_CurrentBanc
int dbg_is_child(void);                // Sequence_IsChildhood: 1 child, 0 adult, -1 unknown
// Guest calls on the game thread, made by the debug frame hook (debug_game.c) at the next frame. dbg_guest_text keeps
// a string in one of four slots and returns its address, or with indirect the address of a pointer to it.
uint64_t dbg_guest_text(int slot, const char *text, int indirect);
int dbg_guest_call(uint64_t fn, const uint64_t a[8]);   // 0 when a call is still queued
int dbg_guest_result(uint64_t *r);                      // 1 and x0 once the queued call ran
uint64_t dbg_player_performer(void);
uint64_t dbg_guest_scratch(void);   // 64 KiB the game may use, for structures passed to guest calls
struct Cpu;
void dbg_game_frame(struct Cpu *c);    // the frame hook, game thread
// Game thread only (from the frame hook): the game's actor warp of the player to pos now. face, when not NULL, is a
// horizontal direction to turn the player to; else the player keeps its rotation.
void dbg_frame_teleport(struct Cpu *c, const float pos[3], const float *face);
// Actors of the generic actor class by name (a heap scan of about a second: helper threads only), and the check that
// an address still holds such an actor.
int dbg_find_actors(const char *name, uint64_t *out, int max);
int dbg_actor_named(uint64_t actor, const char *name);

// Fly, hover and infinite Hover Boots (debug_move.c, run by the frame hook). Fly: the left stick moves where the camera
// looks, X rises, B sinks, A held is three times faster; walls do not stop it. Hover: the height is held where it was
// switched on and the stick walks on air. Infinite Hover Boots: with the Hover Boots on, a fall is stopped where it
// starts and the player walks on air at that height until the boots come off or B is pressed. Fly wins over hover,
// hover over the boots.
void dbg_fly(int on);
int dbg_fly_on(void);
float dbg_fly_speed(void);             // metres per second
void dbg_set_fly_speed(float ms);      // 1..100
void dbg_hover(int on);
int dbg_hover_on(void);
void dbg_hover_boots(int on);
int dbg_hover_boots_on(void);
void dbg_move_frame(struct Cpu *c);

#ifdef __cplusplus
}
#endif
