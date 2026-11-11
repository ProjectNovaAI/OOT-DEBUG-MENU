# Debug menu (F1, Debug tab)

Game build 1.1.1 only (`RT_V111`). The sections below say which parts were tested in game and when.

## Working design (needs in-game verification)
- Position and speed readout, 4 saved positions (teleport writes the actor's position fields only; the game may pull it back).
- Game speed 1/8x to 4x: scales the step written in `frame_rate.c` `ocarina_step_write`. True pause and frame advance are not possible this way, because the physics solver step count reaches 0 below 1/480 s.
- Timer with laps (real time plus the game's 60 Hz ticks) and an on-screen readout.

## Anchors (1.1.1, main base 0x7000000000)
- Player actor: `[[main+0x4e371d0]+0x480]`; name pointer at +0x20 ("Player"); position floats at +0x31c.
- IsPlayerChild: `QueryIsPlayerChild` at 0x7002a386c0 reads `*(main+0x4e7eab0)`, word at +0x4d0, bit 19 (0x80000).
- Flags: `GameData/GameDataList.Product.110.byml.zs`, keyed by MurmurHash3-32 (seed 0) of the name.

## Not built, and why
- **Death barriers, invisible walls, solid colour:** need the Phive/Havok collision data drawn by an own overlay (or the engine's debug draw enabled). Not started.

## Search notes, 2026-11-10 (second pass)
- The binary is stripped and the C++ is heavily inlined; the lookups are only reachable through vtables built by relocations.
- Names found in strings (script/behaviour nodes that call the real code): `OneShotSetGameDataBool/Int/Enum/UInt`, `OneShotSetNonStructGameData*`, `QueryGetGameDataBool/Int/Enum`, `OneShotAddOrRemovePouchContent`, `SeqOneShotChangeScene`, `SeqOneShotChangeScenery`, `OneshotWarpToTargetPosAndRot`, `ExecuteEventPlayerWarpSafePos`, `OneShotPlayerSetSelectedItem`. Their string getters are at 0x7002936d54 (SetGameDataBool), 0x700293aaa4, 0x7002a10e44, 0x70028b51f4, 0x7003bb2cd4, 0x7002968b24, 0x70026c5884; the implementing vtable slots were not identified.
- MurmurHash3 is inlined (constants at 0x70005ae05c, 0x7001176790, 0x7002210e00); no single lookup function was isolated.
- Collision: the engine has layers/tags named `AirWall`, `AirWallOnly`, `AirWallForClimb`, `Barrier`; drawing them needs the Phive rigid-body list, not yet located.
- Next step that does not need more static analysis: a live memory scanner in the menu (value search, then narrow by changed/unchanged) to find the item, rupee and flag addresses while the game runs.

## Change monitor (Debug tab, `runtime/debug_watch.c`; working, tested in game 2026-11-11)
For glitch hunting: switch on "Watch changes", do one thing in game, and the list shows what it touched, with the seconds since the start. Lines also go to `debug_changes.txt` next to the game (rewritten at each start).
- **Flags.** One helper thread reads each flag store whole every 50 ms (`debug_tools.c` `dbg_flag_store`, one memory read per store) and compares every entry's value: bool, int, float, string, vec3 and enum, e.g. `int PouchData.Item_Arrow_Normal.StockNum 50 -> 7`, `string Sequence_CurrentBanc "GerudoFortress_Adult" -> "KokiriForest_Child"`. The key of each entry comes from the store's own table (`dbg_flag_store_keys`). A store that moves (the game loaded again) is taken as a new baseline.
- **Names** come from `debug_flag_names.json` next to the game (`python tools/debug_flag_names.py <game>/debug_flag_names.json`, 7846 of 14997 keys); without it, or for a key it lacks, the line shows `key 1234abcd`.
- **Noise.** A flag that changes more than 24 times is left out from then on (`PlayerStatus.CurrentPos`). "Ignore what has changed so far" leaves out every flag that changed since the start or the last Clear: stand still a few seconds first. "Clear the list" starts comparing from now and keeps the ignored flags.
- **Actors.** A second thread counts the loaded actors by name (`dbg_each_actor`, about a second a pass) and logs the names whose count changed (`actor +1 Enemy_Skulltula (now 2)`). A pass with more than 12 names changed (an area loading) gets one line in the menu and its names in the file.
- Console: `changes [on|off|ignore|clear] [N]` prints the state and the newest N lines (40).
- Tested: an arrow count written from the console, warps between Gerudo Fortress and Kokiri Forest (the area, the reserved change, the age flag, the music, the loading flags and the actors all show).

## Memory scanner (Debug tab, `runtime/debug_scan.c`)
Value search over all read-write guest memory (32-bit integers), narrowed by equals/changed/unchanged/increased/decreased, a hit's value can be overwritten from the menu, and `debug_scan.txt` is written next to the game with each hit's offset from the main module. Use: set the value to look for to what the game shows (rupees, hearts), New search, change it in game, set the new value, Keep: equals. `layout.h` is generated, so it is only found in a real build. Not yet compiled or run.

## Remote console (`runtime/debug_remote.c`)
Off until switched on in Debug > Remote console. A helper thread runs the lines of `debug_cmd.txt` (game working directory; `debug_ready.txt` names it) and writes `debug_out.txt` ending in `END`. Commands are listed in `debug_remote.h` (read, ptrs, chain, find, findptr, scan, next, hits, set, write, player, teleport, speed). A file channel, not a socket: nothing is reachable from the network. Untested.

## Live findings, 2026-11-10 (remote console on the dev build)
- Age is per scene: the area names carry the era (`Market_Adult`, `HyruleField_Adult_00`); crossing eras is a scene change (`CannotWarpToAnotherEra` exists as a string). The word at `*(main+0x4e7eab0)+0x4d0` is recomputed every frame (adult 0x144c0, child 0x104c0 in the saves tried): writing it does nothing.
- While in Hyrule Field the strings `HyruleField_Adult_00` (at 0x4a19e4cc8, a FixedSafeString at 0x4a19e4ca8: char* to the inline text, then 0x40, ...) and `HyruleField_Adult` (0x5ace57060) held the current area; no raw pointer to either string object was found, so the owner references them some other way. Heap addresses change per run.
- `find`/`findptr` report one false hit at the host's own stack (the pattern buffer), ignore addresses near 0x39e01f3c0.
- Snapshot diff (Python ReadProcessMemory of the dev game, memory 0x110000000-0x140000000, adult vs child) is too noisy: 2.7 M changed words as the heap is reloaded per era.

## Tracing with hardware breakpoints (remote console: hwatch / wlog)
`hwatch ADDR LEN [rw]` sets x86 data breakpoints (platform.h plat_hw_watch, Windows) on up to 4 aligned 8-byte pieces for all existing threads; every access is logged by `wlog` with the instruction after it and return addresses found on the stack. `anchor` prints the host address of `dbg_remote_set`; `tools/debug_symres.py` turns a host address into the lifted guest function (`f_7000b25da0+0x782`) using `build-cache/ocarina.map`. Set `NX2NAVI_DEBUG_REMOTE=1` to start the console without the menu. `tools/debug_probe.py` is the command-file helper.
- Result 1: the current area name (`Mini_Market_Adult_00`, then `HyruleField_Adult_00`) lives in an array of 0x50-byte string entries in the heap (…eb828/…eb878 here). It is written by guest function `f_7000b25da0` (offset +0x782), reached from `f_7000b25c80` (looks up a component by type hash 0xC3332AD4 and calls it) <- `f_700074d660` <- `f_70032c9ae0`. The next job is to find which caller of that chain originates the request, i.e. the scene-change request function.

## Execution breakpoints (remote console: hexec)
`hexec HOSTADDR [SLOT]` traps before the instruction at a host address runs (host address = map address of `f_<guest>` + slide from `anchor`), logging the stack like `hwatch`. Do not break on the behaviour-tree executors (`f_700074d660`, `f_700074d7b0`, `f_70032c9ae0`): thousands of hits a second. `dbg_tick` now runs on every present, so `NX2NAVI_DEBUG_REMOTE=1` works with the overlay off.
- Result 2: crossing an area border calls `f_7000b25da0` (it writes the current-area name string) from `f_7000b25c80` <- `f_700074d660` <- `f_70032c9ae0` (a node of the behaviour-tree/flow executor, calls vtable slot 0x138 of its object) <- `f_700074d7b0` <- `f_70014c6a70+0x67` (the node that starts it). `f_7000b25c80` only looks up a component by type hash 0xC3332AD4 and hands it to `f_7000b25da0`.
- Not done: calling the area change from the menu. It needs the lifted ABI (guest registers) and running on the game's own thread.

## Registers at the area write (hexec on `f_7000b25da0`, 2026-11-10)
Lifted functions are `void f_<addr>(Cpu *c)`; at an execution breakpoint the host rcx is that `Cpu *`, so the log shows guest x0..x8 and the strings x1..x3 point to. One crossing of an area border gave x0 = 0x1a14ea5c0 (a component in the heap, vtable main+0x4be8300), x1 = main+0x4df5918 (data section: a pointer to main+0x4984c80 then 0x101), x3 = 0xc6d66ac7 (a hash), x2 = 0. The current-area strings (`Market_Adult_00`, `Mini_Market_Adult_00`, …) are an array of 0x50-byte fixed strings (char*, capacity 0x40, inline text) starting around 0x1a14eb810, right after that component. Still open: which function starts a *request* (the origin of a transition), and how to run a lifted function from the menu on the game's thread.

## The menu button (built 2026-11-11; the controller side is not tested: scripts cannot press pad buttons)
A controller button opens the menu on its Debug tab: D-pad right by default, as the game only talks to Navi with it (LB
was the first choice, but players use it for the item switcher). Settings `debug_menu_pad` (0|1, on by default; Debug
tab toggle "Controller button opens the menu") and `debug_menu_button` (a pad name or `none`; the "Open the menu" row at
the top of the Hotkeys group binds it like a hotkey cell). It is a physical button: `input.c` poll_merged drops it in
`pad_read` before the game's bindings, `input_map.c` `input_debug_gate` sees its press, `ui/menu.cpp` `menu_open_debug`
opens (or closes) the menu. With the menu open the D-pad, the face buttons and the triggers work the menu, so only L, R,
the stick clicks and the paddles also close it (`debug_binds.c` `dbg_menu_button_closes`); B always does. In the menu,
LB and RB, LT and RT (and Q / E) turn the pages; a shoulder button chosen as the menu button only closes the menu
(`ui/menu.cpp`, the tab step).

## The game's pause menu (found 2026-11-11, tested)
The bool flag `Sequence_IsPause` is 1 while the game's pause menu is open (seen with the Equipment and Items pages up,
0 after it closed). `debug_binds.c` `dbg_game_menu_open` reads it (once per 10 ms at most); the menu button and every
bound hotkey key and button go to the game while it is 1, for its cursor and pages. The poll reads the flag only while
the menu button is held or a hotkey is bound, so the flag store search (debug_tools.c) does not start otherwise.

## Hotkeys the player binds (built 2026-11-11; keys tested in game, the controller cells not)
`runtime/debug_binds.c`: 27 tools (fly, hover, Hover Boots, invulnerable, infinite magic and items, reload area, switch
age, save and go to slot 1, game speed down and up, fog, each overlay kind, the readouts, the timer, the speedrun timer
and its split), each with two keys and a controller button, settings `debug_hotkey_<name>` in the bind_ format
(`key:g, pad:dpad_up`). Debug tab, Hotkeys group: the Controls tab's cells and capture window; a key or button taken
from another tool says so. `input.c` poll_merged clears a bound key from the keys held and drops a bound physical button
in `pad_read` (the menu's own pad read keeps them), and runs the tools once the input lock is let go, each with a short
on-screen message. Tested: G bound to fly toggled it with the menu closed and did nothing in the game's pause menu.
Pause and frame step are not bindable: while paused the game thread waits in `dbg_pause_wait` and the poll does not
run (Shift+F2 / Shift+F3 work from the window).

## Save profiles and manual saves (built 2026-11-11, tested in game through the console)
`runtime/save_slots.c` (layout in `save_slots.h`), Debug tab group "Profiles and manual saves", console
`saves [new|make|switch NAME|load NAME|delete NAME|drop NAME]`. Everything is kept in `<game folder>/saves/profiles`:
`active.txt` names the profile in use ("Default" without it), each profile is a folder, `progress.zip` holds a profile's
save while another one is played, `manual/<date> <time> <area>.zip` its manual saves. The zips are the menu's save
export format (`save_share.c` `save_share_export_to`).
- The game reads its save at its start and writes it as it plays, so a switch or a load is staged as an import
  (`fs_host.c` `fs_save_import_stage`) and the menu opens the Restart confirmation (`ui/menu.cpp` `menu_ask_restart`).
  The start that applies the import backs up the save it replaces to `saves/backup <date> <time>`.
- A switch first exports the game's save into the profile in use (`progress.zip`; none when the game has not saved), then
  stages the other profile's `progress.zip`, or an empty save for a profile that has none (`save_share_stage_empty`:
  the game then starts as on a first start).
- Which profile is in use changes only once the switch is the save: the switch leaves `save-import.tag` beside the
  staged import, the start that applies the import renames it to `save-import.tag.done`, and `save_profile_active` takes
  it (`fs_save_import_tag_take`). A new import or a cancel drops the tag, so a cancelled switch keeps the old profile.
  The profile that became active loses its `progress.zip`, which would go stale.
- A manual save and a switch take the save the game last wrote (its autosave, or a save from its System page), not
  progress since.
- Tested on the dev build: a manual save, a new profile, a switch to it (empty save, the game at its first start), the
  switch back (every file of the save byte-identical to a copy taken before), a manual save loaded (identical, and the
  game loaded it into Gerudo Fortress at the same place), deleting a profile (the one in use is refused) and a manual
  save. The menu rows themselves were not looked at on screen.

## Profile picker on the title screen
`runtime/save_title.c`, setting `debug_title_profiles` (default 1, toggle "Choose the profile on the title screen" in the
Profiles group). On the title menu and Load page, R (RB, keyboard R) switches to the next profile and L (LB, keyboard Q)
to the previous one. The bar at the bottom shows the profile and loading status.

When both profiles contain saved slots, switching imports the selected profile into the committed save and its working
copy, then refreshes the game's caption list without restarting. A and B are held while the switch runs so a slot
cannot load halfway through it. Repeated L/R presses select the final requested profile.

Switching to or from a profile without saved slots requires A to confirm a restart; B cancels. The game builds its
Continue/New Game choices at startup, so refreshing captions alone cannot safely update that case. A restart is also
the fallback if the working copy cannot be replaced or the game does not finish refreshing its list.

`save_slots.c` `save_profile_switch_now` stages the import and `fs_host.c` `fs_save_import_now` installs it while mounted,
refusing an open writer or uncommitted changes. The old save is backed up. `debug_warp.c` `save_list_reset` runs the
game's list reset (`main+0xbb0010`) on its frame thread and requests a caption read. The list has six entries and six
second copies; its driver (`main+0x4e5420`) returns to phase 4 after reading them. Loading a slot then reads that
profile's `progress.sav` from the replaced working copy.

### Choose a profile while saving
On the game's Save screen, LB/RB (keyboard Q/R) choose the destination profile. A saves the current session into that
profile and makes it active for future saves; B keeps the current profile. This keeps the running session's progress
instead of importing the destination's older progress. The profile you leave keeps its last committed save, and an
existing destination's previous progress is preserved in its manual saves as "Before save".

`save_slots.c` `save_profile_adopt` prepares those snapshots and changes the active profile without replacing the
mounted working save. `save_title.c` holds confirmation while the snapshots run, then forwards A to the game's Save
screen once preparation succeeds. A failure leaves the confirmation held and reports it in the picker.

## Menu cursor (fixed 2026-11-11, tested)
The menu reopens with the cursor on the row it was left at, on every tab. The row did take the cursor, but ImGui's own
first-item request for the window that had just appeared won at the next frame and put it on the first tab
("Display"); `ui/option_rows.cpp` `take_cursor` drops that request.

## Hotkeys and input display (tested in game 2026-11-10)
Debug tab > On-screen readout > Show inputs draws the merged keyboard and pad state (Switch button bits from `input_poll_ui`) bottom left, and Keyboard hotkeys (off by default) turns on F5 save slot 1, F6 go to slot 1, F7/F8 game speed, F9 timer, F12 reload area (added 2026-11-11; the game reads F12 itself too, so the hotkey takes it only while the hotkeys are on, like F2-F9), each with a short on-screen message. The overlay now draws with the perf overlay off (menu_overlay_frame used to skip the frame). Remote console: `overlay MASK` and `hotkeys 0|1` set them without the menu.

## hknpBody layout (Havok 2025.2, from the reflection table at 0x7004895c40)

Entries are 5 qwords: `{0x70000, type*, name*, packed, owner*}`; the offset is the low 16 bits of `packed`
(which follows the name). Offsets: transform 0x0, flags 0x50, index 0x58, collisionControl 0x5c, timAngle 0x5e,
aabb 0x60, shape 0x78, motionId 0x80, nextAttachedBodyIndex 0x84, firstAttachedConstraint 0x88, flags2 0x8c,
qualityId 0x8d, materialId 0x8e, collisionFilterInfo 0x90, maxContactDistance 0x94, userData 0xb8.
collisionFilterInfo carries the layer, which is what separates AirWall / Barrier bodies from ordinary ones.
Still to find: the world's body array and the camera matrix, needed before any collision shape can be drawn.

## Searching for the physics body list (negative results, so they are not repeated)
- A layer-name table is at about main-heap 0x18b0ab4c0 (AirWall, AirWallForShop, FakeWall, EdgeOfTheWorld ...): 16-byte name, next pointer, two numbers. It mixes several enums, so its numbers are not the filter layer ids.
- `find` on the ASCII names hknpWorld, hknpBodyManager, bodyManager, motionManager, bodyArray finds nothing in the game (only the console's own pattern buffer): the reflection data keeps the type table, not those names.
- Dumping every writable region (about 7 GB) and scanning for a 0xC0-byte hknpBody array (consecutive index at +0x58) finds none; scanning for an orthonormal 3x4 matrix with the player's position as translation at +0x30 and plausible shape/index fields finds copies in animation and snapshot data only, no body. The player's collision is probably a character controller with another layout.
- The position writes of the player actor come from `f_70006823b0` (called by `f_70006822f0` <- `f_7000af8520` / `f_70009501e0+0x8b0`).
- Static analysis (Ghidra through rea) does not work here: it needs JDK 21 and a PE target, the game is ARM64.
Next: break on the reads of a static body's collision filter instead of the player's.

## Static collision from the game data (route to the wall overlay)
`Phive/StaticCompoundBody/<Area>/<Area>.Nin_Ounce_NVN2.bphsc.zs` (1772 files) holds an area's static collision. The files are zstd with the dictionaries inside `Pack/ZsDic.pack.zs` (a SARC of `zs.zsdic`, `pack.zsdic`, ...); `tools/debug_romfs_extract.py` reads a file out of romfs.bin through `romfs/index.json` and decompresses it (`python tools/debug_romfs_extract.py PATH OUT`, run with the installed NX2navi python, which has zstandard).
`Market_Adult` decompresses to 4.1 MB: a `Phive` header (offset table: 11 sections up to 0x13090, then 8 more up to 0x3eb7b0), the area name at 0xcf30, and a Havok tag file (`TAG0`, SDK 20250200) at 0x13090. Next: parse that tag file (type table, then the hknpStaticCompoundShape instances: transform, shape key, collision filter info) and the shapes in `Phive/Shape`.

### Hookshot targets (working, tested in game 2026-11-11)
- Debug tab, View, "Show hookshot targets" (`debug_view_hookshot`, colour `debug_view_color_hookshot`). `runtime/debug_hook.c` reads the area's collision tiles once per area change on its own thread and keeps every polygon whose material has flag bit 44 (StickHookshot; bit 50 is HookshotEffective, which no material uses). The file's header comment has the full layout: the tile header, the per-shape-tag records and material table, the compound instances, `hknpMeshShape` vertex decoding and the convex hulls.
- Flag bit numbers: the game's flag-name list starts at LadderUp = bit 0. A survey of all 16,290 material entries in 7,649 collision files bears it out: ladders carry bits 0, 1 and 3; the Lens of Truth glass 47 and 63; climbable cliffs and nets 40; water walls 59. Material ids follow the list Undefined, Soil, Grass, Sand, ..., Metal = 10, ..., Wood = 14.
- The hookshot's own target objects (`Obj_HookShotTarget_*`, `Obj_*_HookShotTarget_*`, `Land_*_HookTarget_*`, the HookShotFrame / HookShotBase / HookShotDecoration pieces) carry no such flag: being a target belongs to the object. They are found by name among the stage placements and drawn from `Phive/Shape/**/<name>__*.bphsh.zs` at the placement's transform, filled and brighter, with their name.
- A tile's record names a run of materials (first entry and count); a mesh's own shape tag picks within it. The first build added the mesh tag to the record number instead and marked stairs and ground with a neighbour's flags (seen in Gerudo Fortress). Matching the tiles' meshes by geometry to the shape files they copy, whose own material tables are the truth, settles it for every Gerudo object. GerudoFortress_Adult then has 386 surface polygons: ivy 232, wood 140, the metal ladder 12, a wire net 2. Haunted Wasteland: 22 surface polygons and one target object. Console: `hook [RANGE]`.

### Tag file layout (Market_Adult.bphsc, found by hand)
Chunks are big-endian `size|flags<<30` + 4-char tag: `TAG0` > `SDKV`, `DATA`, `TYPE` (`TPTR TST1 TNA1 FST1 TBDY THSH TPAD`), `INDX` > `ITEM`. ITEM entries are 12 bytes little-endian: `type|flags<<28` (0x10 object, 0x20 array), offset in DATA, count. DATA starts 8 bytes after the `DATA` tag. Item 1 is the compound shape (type 1, offset 0); item 2 is an array of 150 `hknpShapeInstance` of 64 bytes at DATA+0xc0: quaternion xyzw (0x00), translation (0x10), scale (0x20), instance index u16 (0x2c), 0xffff, then a relative pointer (from its own address) to the shape (0x30). The collision layer is not in the instance; it is in the shape or its tags (not decoded yet). Type body table (TBDY) encoding not decoded yet.

### Writing the player position does not stick
Tried a "raise 5 m" / "hold height" feature on top of `dbg_player_set_pos`: the write succeeds (the remote console reports done) but the actor position reads back unchanged a frame later, so the actor position is an output of the physics/character controller, not an input. Position teleports (and the saved slots) are therefore unreliable; moving the player needs the character controller's own state, which is not found yet. The feature was reverted.
- Do not set `hexec` on `f_70006823b0` (the actor transform writer): the game exited as soon as the player moved.

### Air walls in the data files
`Phive/Shape/**` holds 5877 shape files (`.bphsh.zs`), named by model: `<Model>__Physical` (4012), `<Model>__Airwall` (436), `AirWall` (5), `CameraAirWall` (3), `AirwallForClimb` (2), plus StairStep/StairSlope. So an invisible wall is a shape file with an Airwall name; the layer is not stored in the area's compound body (`Market_Adult.bphsc` contains no name strings beyond the area name; its 150 instances point at 70 embedded polytope shapes). The link from an instance to a named shape (a name hash in the instance's user data or the shape's properties) is not decoded yet.

### Health (found by hand, game build 1.1.1)
Health is in quarter hearts. A `{current, max}` int pair (52 = 13 hearts) sits in a heap object (it was at 0x17455f6b0 in one run; the address moves between runs). It follows damage (29 -> 21) but is a copy: writing max into it from the console is overwritten back within a frame, and no float form of the hearts matches. A hardware write watch on that pair, set while writing to it from the console, crashed the game. The source the game copies it from is not found yet.
- Hardware write watches on the health copy (`{current, max}` pair, e.g. 0x174564540 at 15/52) crashed the game every time (also four at once froze it). Only the position watch on the player actor has been safe. Do not `hwatch` heap addresses outside the actor; use reads and snapshots.

### Health (working)
Health is stored in quarter hearts in the player's heap data as 16-byte records `{class pointer into main, value, key hash}`; the code reads them as `{value, key, pointer}` 8 bytes in. The health record has key `0x31AB5580` (`PlayerStatus.Life`), the maximum follows 0x10 later with key `0xFBE01DA1` (`PlayerStatus.MaxLife`, 52 = 13 hearts). The other `{cur, 52}` pairs in memory (HUD) are copies that the game rewrites. Found by diffing a memory snapshot (value 21) against the live game (15) and testing the 10 matches by writing 52 and watching the hearts. The entry moves between runs, so `dbg_health_get` finds it once with a helper thread (a 1 MB-chunk read of the heap, about 30 s) by the key and a pointer-into-main check, then re-validates it on every use. Menu: invulnerable (refills every presented frame), hold health at N hearts, refill, one quarter heart, heart containers +/-. Console: `health`, `sethealth N`, `setmaxhealth N`, `god 0|1`, `lock N` (quarter hearts, 0 off). These pairs are the player's own copy; the flag store itself is described under "Flag names and the int flag store".

### Flag names and the int flag store (working, tested in game 2026-11-10)
- **Names.** The flag list holds keys only. `tools/debug_flag_names.py OUT.json` hashes the identifier-like strings of the small romfs files and builds `Struct.Member` names from them: 7846 of the 14997 keys get a name (459 of 1259 ints). Useful ones: `PlayerStatus.Life`, `.MaxLife`, `.MP`, `.MPMax`, `.CurrentRupee`, `.EquipWeapon/Shield/Armor/Boot`; `PouchData.<Item>.StockNum` (`Item_Arrow_Normal`, `Item_Bomb`, `Item_Bombchu`, `Item_DekuStick`, `Item_DekuNut`, `Item_DekuSeed`, `Item_MagicBean`, `Item_Token_Gold`, ...); `IsGet.<Item>` (bool, 199 members); `DungeonInfo.<Dungeon>.SmallKeyNum`, `.IsGetMap`, `.IsGetCompass`, `.IsGetBossKey`; `Sequence_IsChildhood`; the scene-change request `Sequence_ReservedNextBanc`, `Sequence_ReservedNextStartPosName`, `Sequence_ReservedNextPosition`, `Sequence_CurrentBanc`; `WarpPoint_FaroreWind.BancName/.Pos/...`.
- **Int store.** Every int flag is one 0x28-byte entry of a heap array, in flag-list order: class pointer `main+0x4c20718`, the default value at +0x18, the value at +0x1c and again at +0x20. A `{key, index}` table ends right before the first entry (life is index 963, the list position). `debug_tools.c` `flags_locate_thread` finds the first entry (three class pointers 0x28 apart, none before) and `flag_index` reads the table, so nothing depends on indices. Writing an entry works at once for item counts: arrows 50 showed as a full quiver on the HUD.
- **Health and magic are owned by the player.** The player's copy is written over the flag on every frame, so a write to the flag is undone. Health: the `{value, key}` pairs of the earlier section. Magic: an object that keeps the keys of `PlayerStatus.MP`, `PlayerStatus.MPMax` and one more, then the value (`dbg_magic_get`).
- **Not the store (do not repeat):** a table of `{key, value}` pairs that holds every flag is the image of the last save (it kept the old arrow count after a shot); `{value, key, 0}` records after a lone class-pointer node are a queue of recent changes; lists of bare keys belong to behaviour data. Other 4-byte copies of an item count (HUD, behaviour blackboard) follow the store within a frame and undo writes.
- Menu: Magic, Infinite magic, Infinite items (holds rupees and the ammunition at the largest value every frame; console `infitems 0|1`), and a row per counted thing (rupees, arrows, bombs, bombchus, sticks, nuts, seeds, beans, gold tokens, small keys of eight dungeons). Console: `flag NAME [VALUE]` (any int flag by name), `items`, `item INDEX VALUE`, `magic [VALUE]`, `infmagic 0|1`. Tested: arrows, rupees (read back, no HUD to see it), magic and infinite magic. Not tested: counts of things the save does not own, small keys.
- Next: the bool store (same idea, another entry class: `IsGet.*` gives and removes items, `Sequence_IsChildhood`), then the string flags for a warp through `Sequence_ReservedNextBanc`.

### Warps, teleport and the age switch (working, tested in game 2026-11-10)
- **Frame hook.** `recomp/patches-v111.txt` set `dbgframe` replaces `ldr x8, [x0, #0xa8]` at main+0x6e6bf8 (the framework's per-frame update) with a call to the patch page at +0xf70. `runtime/debug_game.c` `debug_frame` makes that load and runs `debug_warp.c` `dbg_game_frame` on the game thread, which may call guest functions (`rt_call`, registers saved around it). The first site tried, `mov x19, x0` at +0x6e6c00, hung the boot: the lifter keeps callee-saved registers in C locals across calls, so a host function at a site must stand in for an instruction that writes a caller-saved register.
- **Teleport.** `f_7000c6fb30(actor, &pose, 0)` is the actor warp the event nodes `OneshotWarpToTargetPosAndRot` / `...YRot` call. The pose is a 3x4 row-major matrix with the translation in the last column. It moves the physics body too, so the game does not pull the player back. `dbg_player_teleport` queues it for the next frame; `dbg_player_set_pos` (the saved positions, console `teleport`) uses it whenever the hook runs.
- **Area change.** A loading zone's event (`Dm_SY_BancChangeGo`) reserves the change in four string flags: `Sequence_ReservedNextBanc` (stage file, e.g. `KakarikoVillage_Adult`), `..NextStartPosName` (entrance actor), `..BootEventName` (arrival event), `..EventStarterName`. The game then loads the named file. A warp watches for a reservation (helper thread every 0.2 ms, and the frame hook) and writes its destination over it. The destination table (135 entrances) is `debug_warp.c` `warps`.
- **Instant warp.** Entrances are actors named `FieldAndDungeonEntranceEventRequestActor` (actor class main+0x4b25e18, name at +0x20, position at +0x31c) at the arrival point. Their trigger is an actor named `Area`, on the floor a few metres away, with `[[area+0x3d8]+0x160]` = the entrance. `dbg_warp_now` finds the triggers in the heap, teleports the player into the nearest one (1 m above its position) and tries the next after two seconds without a reservation. Only actors near the player are loaded, so the nearest zone is often the only one. Teleporting onto the entrance actor itself does nothing: it is not the trigger.
- **Age.** Areas with both ages have a `_Child` and an `_Adult` file, and the file sets the age: a warp into the other file changes the body, the equipment and `Sequence_IsChildhood` (tested Lost Woods adult to Kokiri Forest child, and Kokiri Forest child to adult). "Switch age here" (console `agesw`) warps into the other file of the current area, at the entrance last reserved (the frame hook remembers it) or the file's first.
- **Force age** (console `warpage [area|child|adult]`; tested in game 2026-11-11: child into GerudoFortress_Adult, adult into KokiriForest_Child, child by Reload area in KokiriForest_Child). Holding `Sequence_IsChildhood` while an area loads changes what the game allows, but not the body in an area made for one age. The body comes from the player's resource set (shared, child and adult lists at +0x28/+0x38/+0x48, the child byte at +0x70). `f_7000945610(set, child)` writes the byte and fills in the chosen age's list, and `f_7000943550` builds the set from it. Its three callers pass a byte the area decides. The patch set `dbgage` has two sites: the build's `ldr x8, [x0, #0x20]` at main+0x94358c notes the player's set (`dbg_resource_set`), and `mov w20, w1` at main+0x945620 hands the player's set the forced age (`dbg_resource_age`). Writing only the child byte at the build left the child list empty; naming `Player_Child` into it hung the load. `IsForceSwitchChildOrAdult` and the other age fields of `GameBancParam` are only read by its serialisers (value bytes +0x349, +0x34a, +0x359, set flags +0x3a1 to +0x3a3).
- **Reload area** (tested in game 2026-11-11, Gerudo Valley as child, from the menu, the console and F12): the same warp into the current file at the entrance last reserved (`dbg_reload_target`), or the file's first entrance when none was seen (the save's own spawn is not in the table). For practice: a trick or a route retried from the area's start. On-screen readout > Show area draws the file and that entrance ("GerudoValley_Child from GerudoValley_Entrance"); `dbg_last_entrance` returns them. During the warp the overlay still draws over the fade, which is the game's own scene going black, not a loading screen.
- **Console.** `warps [FILTER]`, `warp INDEX [now]`, `warpcancel`, `agesw`, `reload`, `warpage [area|child|adult]`, `area`, `teleport X Y Z`, `fget NAME`, `fset NAME VALUE` (bool, int, float, enum by name or number, string, vec3 "x,y,z"), `gcall FN ARGS...` (a number, `s:TEXT`, `p:TEXT`, `player`, `perf`, `bufOFF`), `gbuf`, `gres`.
- **Did not work (do not repeat):** requesting `Dm_SY_BancChangeGo` from the player's event performer (`f_70004aa890`) with the strings set; setting the strings and the event sequence by hand. Both stall at `IsWaitEventPlayingAfterChangeBanc` with no player.

### Owned items and the magic level (working, tested in game 2026-11-10)
- An item is two flags: `PouchData.<name>.StockNum` (int: 1 for an item that is not counted, the count for ammunition) and `IsGet.<name>` (bool: ever got; it also skips the first-get scene). Songs have the bool only. `debug_tools.c` `owned` lists 74 things; giving sets both, removing sets the count to 0 and the bool off. Tested: the Hookshot and Longshot leave the item menu and come back, Ice Arrows and Nayru's Love appear.
- Magic: the maximum is `PlayerStatus.MPMax`, 240 a level (none 0, single 240, double 480); `Item_FairyBless_MP` and `Item_FairyBless_MPDouble` record the levels got. Setting the maximum alone changes the meter on screen at once. `dbg_magic_set_level` sets all three.
- Console: `own [FILTER]`, `give INDEX 0|1`, `magiclvl [0|1|2]`.

### Fly, hover and infinite Hover Boots (working, tested in game 2026-11-10)
- `runtime/debug_move.c`, run from the frame hook. Each mode keeps its own position and puts the player there every frame with the actor warp (`dbg_frame_teleport`), so gravity never acts. The game gives a player in the air no walking speed, so all three move the position with the left stick themselves: stick up goes along the view (player minus the `PlayerCamera` actor's position, horizontal), A held is three times faster. Fly also rises on X and sinks on B, and passes through walls; hover and the boots move at 6 m/s at a fixed height.
- The teleport keeps the actor's rotation (`dbg_frame_teleport` reads the axis columns at +0x2f8) unless a facing is given; moving turns the player along the movement.
- Infinite Hover Boots: with `PlayerStatus.EquipBoot` = `Armor_Adult_HoverBoot`, a fall faster than 1.5 m/s starts a hold at the height it started from; B or taking the boots off ends it.
- Tested: fly up 8 m/s and along the view, right on the stick goes right on screen; hover walks across at a constant height; the boots hold stops a fall and walks on air.
- Console: `fly 0|1 [SPEED]`, `hover 0|1`, `hoverboots 0|1`.

### Speedrun timer (working, tested in game 2026-11-11)
- `runtime/debug_igt.c`, ticked on every present (`dbg_tick`). IGT is real time minus the loading screens: it stops while `Sequence_IsLoadingBanc` is set or there is no player actor. The game keeps no play-time flag of its own (searched the flag names: only minigame timers). RTA runs alongside.
- Splits: by hand (F3, menu, console `igt split`) or on every area change (`Sequence_CurrentBanc` differs from the last area seen outside a load), named after the area left. A stopped run that beats the best run's last split becomes the best run on reset, kept as `debug_splits.txt` (one `nanoseconds name` line a split) in the game's folder; every split shows its difference to the same split of the best run.
- Settings (nx2navi.ini `[debug]`, live): `debug_igt` shows the panel (top right, under the item buttons), `debug_igt_autosplit`, `debug_igt_autostart` (starts when a save's player first appears). The menu's switches write them.
- Hotkeys (with the hotkeys on): F2 start/stop, F3 split, F4 reset. Console: `igt [toggle|split|reset]`.
- Tested: auto-start on loading a save; a warp's 4.7 s load left out of IGT; the auto split on an area change; a second run's split shown 25.5 s ahead in green.

### Fog off (working, tested in game 2026-11-11)
- The game has no fog flag; the haze is a volumetric fog computed on the GPU. The renderer already recognises that fog's first compute pass by its shape (`vk_pipeline.c` `fog_find`: a branch whose false side writes the froxel empty with five image writes and returns). With `debug_fog_off` set, `vk_pipeline_compute` gives that shader a second pipeline (key `compute = 2`) built from `spirv_fog_off`, in which the branch's condition is a new `OpConstantFalse`, so every froxel is written empty and the fog pass adds nothing. The switch takes effect on the next dispatch: no restart, no area reload.
- Setting (nx2navi.ini `[debug]`, live): `debug_fog_off`. Menu: Debug tab, View, "Fog off". Console: `fog 0|1`, or any setting with `cfg KEY [VALUE]`.
- Tested in Kokiri Forest (the haze between the trees disappears, the houses across the village show clearly) and in Hyrule Field (the distance haze over the field goes, the mountains and the far trees show sharp); switching back brings it back at once. The scene is a little darker without fog, because the fog also scatters light.
- Side effect seen in game: a faint ghost image under Link, Navi and other characters (the menu text and `nx2navi.ini.example` warn of it, and of effects not known yet).

### Plant draw distance (working, tested in game 2026-11-11)
- Debug tab, View: "Plant draw distance", x0.25 to x8 (`debug_draw_distance`, applies at the next start). `draw_distance.c` multiplies the game's default foliage distances before the guest starts (`boot.c rt_boot`, after `wide_init`).
- Each area's `Foliage/ResFoliageScene/<area>.game__foliage__ResFoliageScene.bgyml` may carry `LodSetting` and `LodSetting_hh` (handheld) with `Dist_*` and `Range_*` keys. Their reader is the field visitor `f_7002bed300` (it uses the key strings at main+0x3de67d3 `Dist_Undergrowth0` and on); a missing key takes its default from nine floats at main+0x3eefd38: Dist_Undergrowth0 40, Dist_Undergrowth1 60, Dist_Ornament 35, Dist_LowTreeShootLeaf 30, Dist_LowTreeShadow 40, Dist_LowTree 135, Range_LowTree01 10, Range_LowTree12 45, Turf_1PolyLen 0.3. Of the 43 scene files only Graveyard (both ages), Kokiri Forest child (handheld only), Hyrule Castle, Zora's River and Zora's Domain set any, so the defaults rule nearly everywhere (Hyrule Field sets none).
- The game copies the table at boot: writing it while the game ran (VirtualProtectEx from outside, then a change of area) changed nothing, even at x0.05. Written before the guest starts it works: at x0.25 the bushes along the wall in front of the Hyrule Field drawbridge (30 to 60 m away) are gone; at x4 there are more plants along the wall and on the far slopes. `draw_distance.c` leaves the table alone if it does not hold the expected values.
- Not covered: the scenery models' own LOD (below) and the areas' own `LodSetting` values.

### Model LOD (searched, not built)
- `System/GameGraphics/ModelLodParam/Default` is one live object (vtable main+0x4b87a88, found with `findptr`): DynamicOffsetForNormal 0x38, DynamicOffsetForStaticDemo 0x3c, FovyScaleForNoraml 0x40, FovyScaleForTimeline 0x44, LodStartDistCoefForConsole 0x48 (1.0), LodStartDistCoefForHandheld 0x4c (0.8), MaxLevelDistScale 0x50 (1.0), StaticOffset 0x54, flags 0x58-0x5f (field visitor `f_70031a2560`). Writing 0.02, 10 or 0 to the coefficients, then reloading Hyrule Field, changed nothing visible, so it is not the lever for the distant scenery.
- Models also carry their own `GameModelController_LodMaxLevelDistScale` / `GameModelController_LodStartDistCoef` (BYML keys read in `f_7000089540`, 1.0 when missing). Foliage shapes have an `ExtendDrawDist` field (`f_7002bf4740`).

### Items at any age (work in progress, 2026-11-11)
- Debug tab, Player: "Items at any age (WIP)", setting `debug_any_age_items`, console `anyage [0|1]`, hotkey `anyage`. Done: the pause menu lets every item and piece of equipment be chosen at either age, live (tested: as child the bow, hammer, hookshot and magic arrows show usable; off, greyed and "Adult Only").
- How: `UI/Pouch/PouchTable/PouchTable.game__ui__PouchTable.bgyml` lists `IsUsableAdult` / `IsUsableChild` per item. Its entry reader main+0x33a2500 (ActorShort +0x38, PouchActorShort +0x40, IsUsableAdult byte +0x48, IsUsableChild +0x49, key-present bytes +0x4a..+0x4d) calls the patch page at +0xf30 in place of its closing `mov w0, w21` (`runtime/debug_ages.c`, `debug_game.c` `debug_pouch_entry`). The table is read once at boot (170 entries); the toggle writes the noted entries.
- Not done: using an item of the other age does nothing. Each age's player pack holds only its own items: `ActorHolderParam` (child: Deku stick, slingshot; adult: hammer, hookshot, longshot, bow), `ShooterParam` (child: seeds, nuts, boomerang; adult: arrows, hookshot chain, hammer shock wave) and `AnimationParam` (the child's four animation sets have no bow, hammer or hookshot clips; the adult's none for the slingshot or boomerang).
- Tried (a test mod, not kept): adding the adult's hold settings to `Player_Child.pack.zs` crashes on save load, even the bow alone; a repack without changes loads fine. The crash is main+0xce9690 (a 0xb0-byte object's factory) getting no memory from the heap of the component `Component/ActorHolderParam/Player_Child...`, whose used size was 0x1850. Raising `InstanceHeapSize` in `RSDB/ActorInfo.Product.111.rstbl.byml.zs` (child 5692448, adult 10245064) for the players and the weapons did not help: that component heap is sized elsewhere. Next: find where that heap's size comes from (the component's creation, frames main+0x2d12230 < main+0x669f00 < main+0x612760), grow it only while the setting is on (the user wants the original sizes when off), then the projectiles, then the animations (about 160 MB of adult animation sets; with them all the save load ran out of memory).

### Volume overlay: invisible walls, loading zones, triggers, hidden things, death barriers (working, tested in game 2026-11-11)
- Debug tab, View: "Show invisible walls", "Show loading zones", "Show trigger zones", "Show hidden things", "Show death barriers"; style wireframe, solid colour or both; solid opacity; a colour per kind (neon pink, green, cyan, yellow, orange, purple, or red, blue, white); names; range 25 m to 1 km. Settings `debug_view_*` (nx2navi.ini `[debug]`, live). Console: `view [RANGE]` lists what is drawn with each volume's projection; `cfg DEBUG_VIEW_WALLS 1` and the like switch kinds for one run.
- Only what you can see (`debug_view_occlude`, added 2026-11-11): the overlay is otherwise drawn through everything. With it on, `vk_dlaa.c` `odepth_probe` copies 270 evenly spaced rows of the view distances the upscaler's motion pass writes (`dlaa_mv.comp` `depth_out`, metres along the view, 0 sky) to a host buffer each frame and, four evaluations later, samples them into a 480 by 270 grid; `debug_tab.cpp` `occ_begin` takes it once a frame. Edges are cut into pieces of about 6 pixels and faces into triangles of about 24 pixels, each drawn only when the point in its middle has a clip w no greater than the scene's distance there times 1.02 plus 0.3 m (clip w is the distance along the view). Names and health bars follow their anchor point. Needs DLAA, DLSS or FSR (the motion pass); without one the row says so and everything is drawn. The grid is a few frames old, so the hidden edge trails a fast camera turn.
- Camera: `vk_debug_cam.c` notes, for every scene draw into the scene depth with a scene colour target, the 0x3500-byte scene context block at c3 (the one most draws use this frame is the camera, as `vk_dlaa.c` does for DLSS) and copies its rows 0-2 (view) and 7-10 (projection) from the uniform snapshot. It works with any upscaler and only while the overlay asks. After 3 frames without a scene draw (a loading screen) there is no camera, so nothing is drawn.
- Volumes: `debug_view.c` rescans the actors every 1.5 s on a helper thread (`dbg_each_actor`) and the menu reads their pose each frame. Actor layout: position +0x31c, placement rotation as three unit columns at +0x328, placement scale at +0x34c. A volume's scale is its full size in metres; the box spans -0.5..0.5 of it across and 0..1 of it upwards from the position (the floor). Checked: the `NavMeshCutCylinderArea` round the player sits on the ground at the player's feet, and walking 4 m back from the drawbridge start into the box drawn as the market's loading zone loaded the market.
- Kinds by actor name: walls `*AirWall*` / `*Airwall*`; loading zones `Area` whose link record (+0x3d8, target at +0x160) points at `FieldAndDungeonEntranceEventRequestActor` (`dbg_area_is_loading_zone`); triggers every other `Area` and `*Area*` volume except NavMeshCut*, ForbidAutoPlacement*, VolumeFog*, LocationArea, AreaBgm*, AreaOverwrite*, EftLoc* (clutter); hidden things `*HidingHole*`, `*Skulltula_Gold*`, `*Heart_Piece*`, `TBox_*`, `*MagicBean*`, `*Hidden*`, `*Secret*`, drawn as a 0.8 m cube. A trigger's label names its target (`Area > EventRequestActor`, `Area > TBox_Field_Iron`).
- Death barriers (`debug_view_death`, default colour red), found in the game data (every stage's placements and the actor packs' shape parameters):
  - `Exterminator`: the kill volume under every area, a live actor at (0, -3975, 0). Its pack's shape is one box of half extents 5975 x 500 x 4975 m centred 500 m over the position, with the surface materials `Material_AirWall` and `DisableRespawn`, so its top, the surface a fall out of the level hits, is at y = -2975. Drawn as its outline and a grid over the top; its label sits on that surface straight under the player with the height and the drop ("y -2975 (3068 m below)"). "Death plane under the world" (`debug_view_kill_plane`, default 1) hides it alone; `debug_tab.cpp` `is_kill_plane` picks it out as the one death barrier over 1 km wide.
  - `MinusObj_EldinHardLavaPlane_A_01`: the lava surfaces (Death Mountain Crater, Dodongo's Cavern, Fire Temple, Gerudo Training Ground, Ganon's Castle). Shape: a 10 x 10 m mesh from y -1 to 0 at the placement, times its scale (the crater's lake is scaled 10: 100 x 100 m). Drawn as outline plus grid; checked from above the crater's lake, the grid lies on the lava.
  - `Obj_Abyss_A_01`: the black bottom drawn in a bottomless pit (model `DgnObj_Small_Bottom_A_01`, physics `Dummy`: it has no collision itself). Marked as "Bottomless pit". 41 placements: Shadow Temple (7, all listed in game), Water Temple, Ice Cavern, Spirit Temple, Gerudo Training Ground, Ganon's Castle, Dodongo's Cavern and others.
  - `Obj_Gerudo_QuickSand_*`: the Haunted Wasteland's quicksand river, marked.
  - Lava planes and pit bottoms are never live actors (pack setting `ConstPassive`: the heap holds the name only in loaded data), so `debug_place.c` reads them from the current area's stage files (`Banc/<area>/<banc>_Static.bcett.byml.zs` and `_Dynamic`, zstd with the `bcett.byml.zsdic` dictionary, BYML version 7) once per area change, on the overlay's helper thread. A placement's `Rotate` is applied as Ry * Rx * Rz: the placements of 3 rotated `Area` actors in the crater match their live axes to 1e-4 that way and miss by about 1 in the other orders.
  - Not separate actors: the Haunted Wasteland's sand falls (`Area` triggers whose events are `HountedWasteland_Field_RestartToEntrance` and `..._RestartToCheckpoint`) and the Gerudo Valley river (a loading zone to Lake Hylia) show as trigger zones and loading zones. `TemporarySaveArea` (Ganon's tower collapse: where a fall puts the player back) shows as a trigger zone.
- Enemy health (`debug_view_health`, "Show enemy health"; tested 2026-11-11 in Gerudo Fortress: three Gerudo thieves 18 / 18, a Skulltula 2 / 2): every actor named `Enemy_*`, `DungeonBoss*` or `*MiddleBoss*` in range gets a bar and "life / max" 2.2 m over its position, green to red. The values are the ones the game's AI query `QueryLifeGetLifeByActor` reads (its execute, `f_7002a487a0`): the life component `f_7002c2f660` = `*(*(actor+0x270)+0x2b8)`, class main+0x4b4c460 (the check `f_7002d0c210` makes), current life at `*(component+0x1c10)+8` (`f_7002d0bcc0`) and the maximum at `*(component+0x1c18)+8` (`f_7002d0bcf0`), both 32-bit. The same path gives the player 48 / 48, Epona 220, a pot 1. `debug_view.c` `dbg_view_health`; console `enemies [RANGE]`. Not checked yet: the bar falling as an enemy takes hits.
- Invisible walls of the collision (added and tested 2026-11-11 in Sacred Forest Meadow: 2294 polygons from 4 tiles and 5 placed models, drawn round the meadow's edge): "Show invisible walls" also draws, in the walls colour, (1) the polygons of the area's `Phive/StaticCompoundBody` tiles whose material id is 27 (`Material_AirWall`; `debug_hook.c` `kept` with `Keep.walls`), and (2) for every placed actor whose name is a model with `Phive/Shape/**/<Model>__*Airwall*` files, those shapes at the placement (`wall_shapes_list` lists them once; `wall_one`). The placed AirWall actors stay boxes (`debug_view.c`). The helper thread reads both sets in one pass over the tiles and reloads when the area or the wanted sets change (`dbg_hook_want` takes `DBG_HOOK_SURFACES | DBG_HOOK_WALLS`); `dbg_hook_polys(..., walls)` returns one set. Console `walls [RANGE]` (as `hook`). Fixed 2026-11-11: `tile` marked only the terrain mesh's polygons as walls, so the air walls inside a tile's compound shapes were filtered out of the wall set and many walls never drew; the flag now covers the whole tile (Sacred Forest Meadow: all 2294 air wall polygons returned). A polygon counts as in range when its middle is within the range plus its own reach, so a long wall whose middle is far off still draws when it comes close, and hookshot surfaces and walls each have their own `POLY_MAX`.
- Drawing: ImGui background draw list, the picture's 16:9 rectangle fitted in the window, edges and faces cut at the near plane (clip w 0.05). A filled volume the eye is inside (eye solved from the view-projection) gets edges only, or its faces would tint the whole picture. Nothing is depth-tested: volumes show through walls.

### Settings kept from one start to the next (2026-11-11)
- Every Debug tab setting is a `debug_*` setting of nx2navi.ini [debug], read on first use and written when it changes (`debug_tools.c` `dbg_keep`). Added to the ones kept before (overlay, timer, cheats, binds): `debug_health_lock`, `debug_speed`, `debug_readout` (bit mask), `debug_hotkeys`, `debug_remote`, `debug_hover_boots`, `debug_fly_speed`, `debug_force_age`. Not kept, on purpose: Fly (no clip) and Hover (a start in the air or in a wall), warps waiting, the timers' running state, the saved positions, the memory scanner and the change monitor (work of one session).
