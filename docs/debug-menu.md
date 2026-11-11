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
