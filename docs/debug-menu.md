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
