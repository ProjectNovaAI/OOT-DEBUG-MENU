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
- **Child/adult in an area made for one age:** the area's file decides the player's body. `Sequence_IsChildhood` can be held over the area's choice while it loads (`warpage`, "Age flag on arrival"), which changes what the game allows, but the body stays the area's. The body comes from the stage's player parameters (`Player_Child` / `Player_Adult` over `Player_Base`, resolved in `f_7000944800`); `DebugParam.IsSetChildhoodManually` did not change it.
- **Death barriers, invisible walls, solid colour:** need the Phive/Havok collision data drawn by an own overlay (or the engine's debug draw enabled). Not started.

## Search notes, 2026-11-10 (second pass)
- The binary is stripped and the C++ is heavily inlined; the lookups are only reachable through vtables built by relocations.
- Names found in strings (script/behaviour nodes that call the real code): `OneShotSetGameDataBool/Int/Enum/UInt`, `OneShotSetNonStructGameData*`, `QueryGetGameDataBool/Int/Enum`, `OneShotAddOrRemovePouchContent`, `SeqOneShotChangeScene`, `SeqOneShotChangeScenery`, `OneshotWarpToTargetPosAndRot`, `ExecuteEventPlayerWarpSafePos`, `OneShotPlayerSetSelectedItem`. Their string getters are at 0x7002936d54 (SetGameDataBool), 0x700293aaa4, 0x7002a10e44, 0x70028b51f4, 0x7003bb2cd4, 0x7002968b24, 0x70026c5884; the implementing vtable slots were not identified.
- MurmurHash3 is inlined (constants at 0x70005ae05c, 0x7001176790, 0x7002210e00); no single lookup function was isolated.
- Collision: the engine has layers/tags named `AirWall`, `AirWallOnly`, `AirWallForClimb`, `Barrier`; drawing them needs the Phive rigid-body list, not yet located.
- Next step that does not need more static analysis: a live memory scanner in the menu (value search, then narrow by changed/unchanged) to find the item, rupee and flag addresses while the game runs.

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

## Hotkeys and input display (tested in game 2026-11-10)
Debug tab > On-screen readout > Show inputs draws the merged keyboard and pad state (Switch button bits from `input_poll_ui`) bottom left, and Keyboard hotkeys (off by default) turns on F5 save slot 1, F6 go to slot 1, F7/F8 game speed, F9 timer, each with a short on-screen message. The overlay now draws with the perf overlay off (menu_overlay_frame used to skip the frame). Remote console: `overlay MASK` and `hotkeys 0|1` set them without the menu.

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
- **Console.** `warps [FILTER]`, `warp INDEX [now]`, `warpcancel`, `agesw`, `warpage [area|child|adult]`, `area`, `teleport X Y Z`, `fget NAME`, `fset NAME VALUE` (bool, int, float, enum by name or number, string, vec3 "x,y,z"), `gcall FN ARGS...` (a number, `s:TEXT`, `p:TEXT`, `player`, `perf`, `bufOFF`), `gbuf`, `gres`.
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
