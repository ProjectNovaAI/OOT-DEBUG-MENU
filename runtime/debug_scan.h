// Live memory scanner of the Debug tab (debug_scan.c): finds where the game keeps a number (rupees, hearts, a flag, the
// player's age) by searching for its value, then narrowing the hits as the value changes in game. 32-bit integers.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SCAN_EQUAL, SCAN_CHANGED, SCAN_UNCHANGED, SCAN_INCREASED, SCAN_DECREASED };

// New search over all read-write guest memory for 32-bit values equal to value. Returns the number of hits.
int dbg_scan_first(int32_t value);
// Keeps the hits that match: SCAN_EQUAL the given value, the others compared with the value seen at the last scan.
int dbg_scan_next(int mode, int32_t value);
void dbg_scan_reset(void);
int dbg_scan_count(void);
int dbg_scan_truncated(void);                              // the first scan hit more than the table holds
int dbg_scan_get(int i, uint64_t *addr, int32_t *now);     // 0 when i is out of range
int dbg_scan_write(int i, int32_t value);                  // 1 when written
// Writes the hits (and their offsets from the main module) to debug_scan.txt in the working directory.
int dbg_scan_dump(void);

#ifdef __cplusplus
}
#endif
