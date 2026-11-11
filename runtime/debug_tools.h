// Debug tools of the in-game menu's Debug tab (debug_tools.c, ui/debug_tab.cpp): the player's position and speed, saved
// positions to return to, a game-speed scale, a timer with laps and the on-screen readout.
//
// Only game build 1.1.1 is supported (host.h RT_V111): its anchors into the guest's memory are the ones frame_rate.c
// already uses. On any other build dbg_available() is 0, every reader fails and every writer does nothing.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DBG_SLOTS = 4, DBG_LAPS = 8 };

// The debug tools work on this game build (the memory anchors are known).
int dbg_available(void);

// Opposite-age skeleton proportions, paired around the player model world-matrix calculation.
void dbg_lank(int on);
int dbg_lank_on(void);
int dbg_lank_before(uint64_t model);
void dbg_lank_after(void);

// Items at any age (debug_ages.c): every item of the pause menu's table usable as child and as adult, the setting
// debug_any_age_items; dbg_pouch_entry is the table reader's hook (debug_game.c), the tick applies a change.
void dbg_any_age_items(int on);
int dbg_any_age_items_on(void);
void dbg_any_age_items_tick(void);
void dbg_pouch_entry(uint64_t entry);
int dbg_pouch_entries(void);   // table entries noted so far
int dbg_pouch_entry_at(int i, uint64_t *at, uint8_t own[6]);   // entry i and its own bytes +0x48..+0x4d; 0 past the end

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
// A Debug tab setting kept from one start to the next: key (NX2NAVI_DEBUG_...) set to value and nx2navi.ini written
// soon, when it changed.
void dbg_keep(const char *key, const char *value);
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
// Pause and frame advance: while paused the game thread waits at the start of its frame (dbg_pause_wait, called by the
// frame hook); dbg_step lets n frames through (pausing first if needed). dbg_paused_frames counts the frames stepped
// since the pause began.
int dbg_paused(void);
void dbg_pause_set(int on);
void dbg_step(int n);
uint64_t dbg_paused_frames(void);
void dbg_pause_wait(void);
void dbg_set_time_scale(float scale);

// Timer: real time since start, in nanoseconds, with laps; and the game's own 60 Hz ticks over the same span.
void dbg_timer_toggle(void);       // start or stop
void dbg_timer_reset(void);        // zero, stopped, laps cleared
void dbg_timer_lap(void);          // keeps the current time as the next lap
int dbg_timer_running(void);
uint64_t dbg_timer_ns(void);
uint64_t dbg_timer_ticks(void);
int dbg_timer_laps(uint64_t ns[DBG_LAPS]);   // the number of laps, their times in order

// Speedrun timer (debug_igt.c): in-game time without the loading screens, real time, splits by hand or on every area
// change, each compared with the best run (kept in debug_splits.txt). Shown on screen while the setting
// NX2NAVI_DEBUG_IGT is on; NX2NAVI_DEBUG_IGT_AUTOSPLIT and NX2NAVI_DEBUG_IGT_AUTOSTART (start when a save's player first
// appears) are settings too. Hotkeys F2 start and stop, F3 split, F4 reset.
enum { DBG_SPLITS = 64 };
int dbg_igt_enabled(void);
int dbg_igt_autosplit(void);
int dbg_igt_autostart(void);
void dbg_igt_tick(uint64_t now_ns);
void dbg_igt_toggle(void);
int dbg_igt_running(void);
void dbg_igt_split(void);
int dbg_igt_reset(void);              // 1 when the finished run became the best
void dbg_igt_times(uint64_t *igt, uint64_t *rta);
int dbg_igt_splits(void);
int dbg_igt_split_at(int i, uint64_t *ns, char name[48], int64_t *delta, int *has_delta);
int dbg_igt_best_count(void);
void dbg_igt_clear_best(void);

// On-screen readout lines (bit mask).
enum { DBG_SHOW_POS = 1, DBG_SHOW_SPEED = 2, DBG_SHOW_TIMER = 4, DBG_SHOW_INPUT = 8, DBG_SHOW_AREA = 16 };
int dbg_overlay_mask(void);
void dbg_overlay_set(int mask);

// Hotkeys on the keyboard while the menu is closed (off by default; the Debug tab switches them): F5 saves the position in
// slot 1, F6 goes to it, F7 and F8 lower and raise the game speed, F9 starts and stops the timer, F12 reloads the area
// (dbg_reload_area). dbg_hotkey is called with the function key's number by the window code; it returns 1 when it used
// the key. dbg_toast is the last message to show.
// Shift+F2 pauses and resumes, Shift+F3 steps one frame: the window code passes DBG_SHIFT + the key's number.
enum { DBG_SHIFT = 100 };
int dbg_hotkeys(void);
void dbg_hotkeys_set(int on);
int dbg_hotkey(int fkey);
const char *dbg_toast(uint64_t now_ns);   // NULL when there is none to show
void dbg_toast_show(const char *text);    // a message under the readout for two seconds
float dbg_speed_step(int dir);            // the game speed a step down (-1) or up (1) the F7 / F8 list; the new speed

// Hotkeys the player binds (the Debug tab's Hotkeys group, settings debug_hotkey_<name> = "key:g, key:f9, pad:dpad_up"):
// up to two keys and one controller button per action, read by the game's input poll (input.c poll_merged) while the
// menu is closed. A bound key or button no longer reaches the game: D-pad up bound to Fly is no longer the game's D-pad
// up. While the game's own pause menu is open (dbg_game_menu_open) the bound keys and buttons go to the game as before,
// for its pages and cursor. Pads are physical buttons (PAD_ ids of input_map.h), keys KEY_ ids.
typedef struct { const char *name, *label, *desc; } DbgBindInfo;
enum { DBG_BIND_KEYS = 2 };
int dbg_bind_count(void);
const DbgBindInfo *dbg_bind_info(int i);
void dbg_bind_get(int i, uint8_t keys[DBG_BIND_KEYS], uint8_t *pad);   // from the setting; 0 unbound
void dbg_bind_set(int i, const uint8_t keys[DBG_BIND_KEYS], uint8_t pad);   // writes the setting (no file write)
// input.c, under its lock: dbg_bind_pads is the physical buttons to hold back from the game (1 << PAD_ id);
// dbg_bind_poll takes the keys held (KEY_COUNT entries, NULL when the window is not in front, bound ones cleared for the
// game) and the buttons held, and returns the actions pressed this poll (1 << action) for dbg_bind_run, called after
// the lock is let go.
uint32_t dbg_bind_pads(void);
uint64_t dbg_bind_poll(uint32_t pads, uint8_t *keys, int menu_open);
void dbg_bind_run(uint64_t actions);
// The game's pause menu (the item, map, quest and equipment pages) is open: the menu button and the bound buttons are
// the game's then.
int dbg_game_menu_open(void);
// The controller button that opens the menu on its Debug tab (settings debug_menu_pad, on by default, and
// debug_menu_button, a pad name, D-pad right by default: the game only talks to Navi with it). A PAD_ id, 0 when switched
// off or unset. dbg_menu_button_set writes the button (0 for none). dbg_menu_button_closes: the menu itself has no use
// for this button, so it closes the menu again; the D-pad, the face buttons, the triggers, Start and Back are the
// menu's own while it is open (B closes it).
int dbg_menu_button(void);
void dbg_menu_button_set(int pad);
int dbg_menu_button_closes(int pad);

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
// A whole store, for the change monitor (debug_watch.c): its first entry, entry count, entry size and value offset; 0
// while it is not found. dbg_flag_store_keys fills key_of[i] with entry i's key (0 when the table has none).
int dbg_flag_store(int type, uint64_t *base, int *count, uint32_t *size, uint32_t *value_off);
void dbg_flag_store_keys(int type, uint32_t *key_of, int count);
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
// The Force age patch sites (debug_game.c, set dbgage). dbg_resource_set is called with an actor's resource set just
// before the game builds it and notes the player's set. dbg_resource_age is called when the game chooses a set's age
// (child is 1 or 0) and returns the age to use: the forced one for the player's set, so its body follows (debug_warp.c).
void dbg_resource_set(uint64_t set);
uint32_t dbg_resource_age(uint64_t set, uint32_t child);
int dbg_warp_get_age(void);
// Child and adult switch: an instant warp into this area's file for the other age, at the entrance the player last came
// in by when that file has it. dbg_age_switch_target is that warp, -1 when the area has one age only.
int dbg_age_switch_target(void);
int dbg_age_switch(void);
// Reload area: this area again from the entrance the player last came in by (else its first one), same age.
// dbg_reload_target is that warp, -1 when the area is not in the warp list.
int dbg_reload_target(void);
int dbg_reload_area(void);
// The area and entrance the player last came in by (the stage file and its start position name). 0 before the first
// area change was seen.
int dbg_last_entrance(char banc[64], char start[64]);
const char *dbg_warp_note(void);       // what the warp did last, for the menu
int dbg_current_area(char out[64]);    // Sequence_CurrentBanc
int dbg_is_child(void);                // Sequence_IsChildhood: 1 child, 0 adult, -1 unknown
// Guest calls on the game thread, made by the debug frame hook (debug_game.c) at the next frame. dbg_guest_text keeps
// a string in one of four slots and returns its address, or with indirect the address of a pointer to it.
uint64_t dbg_guest_text(int slot, const char *text, int indirect);
int dbg_guest_call(uint64_t fn, const uint64_t a[8]);   // 0 when a call is still queued
int dbg_guest_result(uint64_t *r);                      // 1 and x0 once the queued call ran
// The title screen's Load list (debug_warp.c): the game reads its save list again, as at its start, from the save
// folder as it is now. dbg_save_list_reload queues it for the next frame (0: the list is not there, or a reload is
// queued already); dbg_save_list_state: 1 read (or nothing asked), 0 queued or reading, -1 refused (the list was still
// reading, try again).
int dbg_save_list_reload(void);
int dbg_save_list_state(void);
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
// Every actor of that class with its name; fn returns 0 to stop. Helper threads only (a heap scan).
int dbg_each_actor(int (*fn)(uint64_t actor, const char *name, void *ctx), void *ctx);
// 1 when an Area trigger is a loading zone (its link leads to the area's entrance actor).
int dbg_area_is_loading_zone(uint64_t area);
// The actor an Area trigger acts on (its link's target: an entrance, an event, another Area, a chest...), 0: none.
uint64_t dbg_area_target(uint64_t area);

// Foliage draw distance (draw_distance.c): multiplies the game's default grass, bush, flower and low tree distances by
// NX2NAVI_DEBUG_DRAW_DISTANCE before the guest starts (boot.c rt_boot); a change applies at the next start.
void dbg_draw_distance_init(void);

// Volume overlay (debug_view.c): the placed volumes and objects the game does not draw, for the menu's overlay
// (ui/debug_tab.cpp debug_overlay_draw). Kinds, as bits of the wanted set:
enum {
    DBG_VOL_WALL = 1,      // invisible walls: actors named *AirWall* / *Airwall* (the collision ones: debug_hook.c)
    DBG_VOL_LOAD = 2,      // loading zones: Area triggers that lead to an entrance
    DBG_VOL_TRIGGER = 4,   // every other Area trigger and *Area* volume (events, camera, music, spawning, horse jumps)
    DBG_VOL_HIDDEN = 8,    // hidden things: grotto covers, Gold Skulltulas, heart pieces, chests, hidden-hole markers
    DBG_VOL_DEATH = 16,    // death barriers and hazards: the kill volume under the world, lava surfaces, pits, quicksand
    DBG_VOL_KINDS = 5,
    // not a volume, so not among the DBG_VOL_KINDS colours: enemies (Enemy_*, *Boss*), drawn as their health
    // (dbg_view_health)
    DBG_VOL_ENEMY = 32,
};
enum { DBG_SHAPE_BOX, DBG_SHAPE_CYLINDER, DBG_SHAPE_MARKER };
typedef struct {
    float pos[3];      // the actor's position (+0x31c)
    float axes[9];     // its placement rotation, three unit columns (+0x328)
    float scale[3];    // a box's or cylinder's full size in metres (the placement scale at +0x34c, times the base
                       // shape's size for the death barriers)
    int kind;          // one DBG_VOL_ bit
    int shape;         // DBG_SHAPE_
    char name[48];
    char target[48];   // an Area trigger: the name of the actor it acts on (its link), else empty
} DbgVolume;
// One placed actor of a stage file (debug_place.c): its name (valid during the callback only), position, rotation as three
// unit columns, and placement scale.
typedef struct {
    const char *name;
    float pos[3], axes[9], scale[3];
} DbgPlacement;
// Calls fn for each actor placed in the stage files of banc (e.g. "DeathMountainCrater_Child"), Static then Dynamic, until
// fn returns 0. The count seen, -1 when the area has no stage files.
int dbg_place_each(const char *banc, int (*fn)(const DbgPlacement *, void *), void *ctx);
// A zstd-compressed romfs file decompressed with the dictionary `dict` of Pack/ZsDic.pack.zs ("bcett.byml.zsdic",
// "zs.zsdic"), malloc'd; NULL when path is NULL, missing or does not decompress (debug_place.c).
uint8_t *dbg_romfs_unzs(const char *path, const char *dict, size_t *n_out);
// The kinds to collect (0 stops the helper thread's rescans). The list is rescanned every 1.5 s while any is wanted.
void dbg_view_want(int kinds);
// The volumes of the wanted kinds within range metres of the player, with their positions read now; at most max.
int dbg_view_volumes(DbgVolume *out, int max, float range);
// The kinds the settings switch on (debug_view_walls, _loading, _triggers, _hidden, _death, _health); keeps
// dbg_view_want in step.
int dbg_view_kinds(void);
// Enemy health (debug_view_health): an actor's life component is *(*(actor+0x270)+0x2b8), of class main+0x4b4c460;
// its current life is the 32-bit value at *(component+0x1c10)+8 and the maximum at *(component+0x1c18)+8 (the game's
// own life query, QueryLifeGetLifeByActor, reads them so). Units are the game's: 2 for a Skulltula, 18 for a Gerudo
// thief, quarter hearts for the player.
typedef struct {
    float pos[3];      // the actor's position (+0x31c)
    int life, max;
    char name[48];
} DbgHealth;
// The enemies within range metres of the player that have a life component, read now; at most max.
int dbg_view_health(DbgHealth *out, int max, float range);

// Hookshot targets (debug_hook.c, debug_view_hookshot): the surfaces the hookshot sticks to, from the area's collision
// files (material flag StickHookshot), and the target objects placed in its stage files, drawn from their collision
// shapes. Invisible walls (debug_view_walls), from the same files: the polygons of material AirWall and the air wall
// shapes of the placed models. Read on a helper thread once per area change while wanted.
enum { DBG_VOL_HOOK = 64 };   // not a volume kind either: the set dbg_view_kinds returns has it for the overlay
typedef struct {
    float p[8][3];     // a convex polygon's corners in world space
    int n;             // 3 to 8
    int target;        // 1: part of a target object, 0: a surface of the level
    int wall;          // 1: an invisible wall polygon, 0: a hookshot one
} DbgHookPoly;
typedef struct {
    float pos[3];
    char name[48];     // the target object's actor name
    int shaped;        // 0: no collision shape was found for it, so the overlay marks its position
} DbgHookLabel;
enum { DBG_HOOK_SURFACES = 1, DBG_HOOK_WALLS = 2 };
void dbg_hook_want(int want);   // DBG_HOOK_ bits, 0 stops the helper thread
// The hookshot polygons (walls 0) or the invisible wall ones (walls 1) within range metres of the player (by their
// middle), nearest kept when more than max are in range.
int dbg_hook_polys(DbgHookPoly *out, int max, float range, int walls);
int dbg_hook_labels(DbgHookLabel *out, int max, float range);   // the target objects within range
void dbg_hook_status(char *out, size_t n);   // what was read for the area, for the menu and the console

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
