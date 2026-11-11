// Foliage draw distance (debug menu, View; NX2NAVI_DEBUG_DRAW_DISTANCE, game build 1.1.1): how far out the grass tufts,
// bushes, flowers and low trees are drawn. Each area's Foliage/ResFoliageScene/<area>.game__foliage__ResFoliageScene
// file may set a LodSetting (and LodSetting_hh for handheld) of Dist_* and Range_* keys; the reader of those keys
// (f_7002bed300, a field visitor) takes each missing key from a table of defaults in main's read-only data, and most
// areas set none (Hyrule Field, Gerudo Valley, Kakariko...; Graveyard, Kokiri Forest, Hyrule Castle and Zora's River
// and Domain set a few). The table, at main+0x3eefd38, nine floats in the visitor's order: Dist_Undergrowth0 40,
// Dist_Undergrowth1 60, Dist_Ornament 35, Dist_LowTreeShootLeaf 30, Dist_LowTreeShadow 40, Dist_LowTree 135,
// Range_LowTree01 10, Range_LowTree12 45 (metres), Turf_1PolyLen 0.3. The game copies the table at boot, so
// dbg_draw_distance_init multiplies the eight distances before the guest starts; a change applies at the next start.
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define FOLIAGE_DEFAULTS 0x3eefd38ull
#define DISTANCES 8

static const float kGame[DISTANCES] = {40, 60, 35, 30, 40, 135, 10, 45};

void dbg_draw_distance_init(void) {
    if (!RT_V111) return;
    double k = cfg_double("NX2NAVI_DEBUG_DRAW_DISTANCE");
    if (!(k > 0) || k == 1.0) return;
    uint64_t at = MOD_MAIN_BASE + FOLIAGE_DEFAULTS, end;
    float now[DISTANCES], v[DISTANCES];
    memcpy(now, (const void *)(uintptr_t)at, sizeof now);
    if (memcmp(now, kGame, sizeof now)) {   // not the table this build was measured on
        rt_log("draw distance: the foliage distances at main+0x%llx are not the game's own; left as they are",
               (unsigned long long)FOLIAGE_DEFAULTS);
        return;
    }
    for (int i = 0; i < DISTANCES; i++) v[i] = (float)(kGame[i] * k);
    uint64_t page = at & ~0xFFFull, last = (at + sizeof v - 1) & ~0xFFFull;
    int prot = plat_mem_state(page, last + 0x1000, &end);
    if ((prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) || end < last + 0x1000) return;
    if (prot == PLAT_PROT_READ && !plat_mem_protect(page, last + 0x1000 - page, PLAT_PROT_RW)) return;
    memcpy((void *)(uintptr_t)at, v, sizeof v);
    if (prot == PLAT_PROT_READ) plat_mem_protect(page, last + 0x1000 - page, PLAT_PROT_READ);
    rt_log("draw distance: foliage x%.2f (undergrowth %.0f / %.0f m, low trees %.0f m)", k, v[0], v[1], v[5]);
}
