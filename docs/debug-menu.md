# Debug menu (F1, Debug tab)

Game build 1.1.1 only (`RT_V111`). Untested in-game as of 2026-11-10: it was written and syntax-checked, not run.

## Working design (needs in-game verification)
- Position and speed readout, 4 saved positions (teleport writes the actor's position fields only; the game may pull it back).
- Game speed 1/8x to 4x: scales the step written in `frame_rate.c` `ocarina_step_write`. True pause and frame advance are not possible this way, because the physics solver step count reaches 0 below 1/480 s.
- Timer with laps (real time plus the game's 60 Hz ticks) and an on-screen readout.

## Anchors (1.1.1, main base 0x7000000000)
- Player actor: `[[main+0x4e371d0]+0x480]`; name pointer at +0x20 ("Player"); position floats at +0x31c.
- IsPlayerChild: `QueryIsPlayerChild` at 0x7002a386c0 reads `*(main+0x4e7eab0)`, word at +0x4d0, bit 19 (0x80000).
- Flags: `GameData/GameDataList.Product.110.byml.zs`, keyed by MurmurHash3-32 (seed 0) of the name.

## Not built, and why
- **Child/adult switch:** the setter of bit 19 was not found. Other writers of +0x4d0 touch masks 0x10000, 0x2000 and 0x180 only, and a scan for orr/and of 0x80000 near an access to +0x4d0 found nothing. The age change likely goes through a different path (save data copy or a helper function). Writing the bit directly would not change the loaded Link model or the save.
- **Warp, items, flags:** need the game's scene-change request and the GameDataMgr get/set functions, not yet located.
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
