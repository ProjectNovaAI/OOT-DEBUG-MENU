// Remote console of the Debug tab (debug_remote.c): a helper thread runs the commands a tool drops into debug_cmd.txt in
// the game's working directory and writes the answers to debug_out.txt (last line "END"). Off until switched on in the menu.
// Commands: main, regions, read ADDR [LEN], ptrs ADDR [N], chain ADDR OFF..., str ADDR, write ADDR HEX, find HEX [rw],
// findptr VALUE, scan INT, scanf FLOAT, next eq|changed|same|up|down [INT], hits [N], set INDEX INT, player,
// teleport X Y Z, speed S. Addresses are the guest's (equal to host addresses).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void dbg_remote_set(int on);
int dbg_remote_enabled(void);

#ifdef __cplusplus
}
#endif
