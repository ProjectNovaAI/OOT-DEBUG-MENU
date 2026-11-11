// Items at any age (Debug tab, Player, "Items at any age (WIP)"; setting debug_any_age_items). Work in progress: this
// unlocks the pause menu only. Using an item made for the other age does nothing, as each age's player pack
// (Pack/Actor/Player_Child, Player_Adult) holds only its own items' models (ActorHolderParam), projectiles
// (ShooterParam) and animations (AnimationParam). The pause menu's item table
// (UI/Pouch/PouchTable/PouchTable.game__ui__PouchTable.bgyml) lists, per item, IsUsableAdult and IsUsableChild; an item
// without the flag of the current age is greyed out and cannot be put on a button. Each table entry is read by
// main+0x33a2500 (the entry type's field visitor): ActorShort at +0x38, PouchActorShort at +0x40, IsUsableAdult at
// +0x48, IsUsableChild at +0x49, then one "key present" byte per field at +0x4a to +0x4d in the same order. The visitor
// calls the patch page at +0xf30 in place of its last `mov w0, w21` (set dbgframe, debug_game.c debug_pouch_entry),
// with the entry in x20: dbg_pouch_entry notes each entry with its own two bytes. With the setting on, every noted
// entry says usable at both ages; off, each gets its own values back. An entry is written only while it still holds
// the names it had when it was read, so memory the table gave back is left alone.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"

#define ENTRY_NAMES 0x38   // ActorShort, PouchActorShort: 8 bytes each
#define ENTRY_USABLE 0x48  // IsUsableAdult, IsUsableChild, then the four key-present bytes
#define ENTRIES_MAX 512

typedef struct {
    uint64_t at, names[2];
    uint8_t own[6];   // the entry's bytes at +0x48 to +0x4d as read
} PouchEntry;

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PouchEntry g_entries[ENTRIES_MAX];
static int g_n;
static _Atomic int g_on = -1;   // -1: the setting not read yet
static int g_applied;           // what the entries hold now (g_lock)

int dbg_any_age_items_on(void) {
    int on = atomic_load(&g_on);
    if (on < 0) atomic_compare_exchange_strong(&g_on, &on, cfg_bool("NX2NAVI_DEBUG_ANY_AGE_ITEMS") != 0), on = atomic_load(&g_on);
    return on;
}

static int writable(uint64_t at) {
    uint64_t end;
    return plat_mem_state(at & ~0xFFFull, (at & ~0xFFFull) + 0x2000, &end) == PLAT_PROT_RW;
}

// g_lock held: the entry still holds its names and two flag bytes that are 0 or 1
static int intact(const PouchEntry *e) {
    uint64_t names[2];
    uint8_t b[6];
    if (!plat_mem_read(e->at + ENTRY_NAMES, names, sizeof names) || memcmp(names, e->names, sizeof names)) return 0;
    if (!plat_mem_read(e->at + ENTRY_USABLE, b, sizeof b)) return 0;
    for (int i = 0; i < 6; i++)
        if (b[i] > 1) return 0;
    return writable(e->at + ENTRY_USABLE);
}

// g_lock held
static void put(const PouchEntry *e, int any) {
    uint8_t b[6];
    memcpy(b, e->own, sizeof b);
    if (any) b[0] = b[1] = 1, b[3] = b[4] = 1;   // both ages, and both keys present
    plat_mem_prepare_write(e->at + ENTRY_USABLE, sizeof b);
    memcpy((void *)(uintptr_t)(e->at + ENTRY_USABLE), b, sizeof b);
}

void dbg_pouch_entry(uint64_t at) {
    PouchEntry e = {at};
    if (!at || !plat_mem_read(at + ENTRY_NAMES, e.names, sizeof e.names) || !plat_mem_read(at + ENTRY_USABLE, e.own, sizeof e.own))
        return;
    for (int i = 0; i < 6; i++)
        if (e.own[i] > 1) return;   // not an entry as laid out above
    plat_mutex_lock(&g_lock);
    int k = 0;
    while (k < g_n && g_entries[k].at != at) k++;
    if (k == ENTRIES_MAX) {   // full: entries no longer intact make room
        int m = 0;
        for (int i = 0; i < g_n; i++)
            if (intact(&g_entries[i])) g_entries[m++] = g_entries[i];
        g_n = m, k = m;
    }
    if (k < ENTRIES_MAX) {
        g_entries[k] = e;   // read again (a new table in the same memory): its own values are the ones just read
        if (k == g_n) g_n++;
        if (dbg_any_age_items_on() && writable(at + ENTRY_USABLE)) put(&g_entries[k], 1);
    }
    plat_mutex_unlock(&g_lock);
}

// The entries follow the setting (debug_tools.c dbg_tick, every frame; cheap when nothing changed).
void dbg_any_age_items_tick(void) {
    int on = dbg_any_age_items_on();
    plat_mutex_lock(&g_lock);
    if (on != g_applied) {
        int done = 0;
        for (int i = 0; i < g_n; i++)
            if (intact(&g_entries[i])) put(&g_entries[i], on), done++;
        g_applied = on;
        rt_log("debug items: %s at any age (%d of %d table entries)", on ? "usable" : "back to their own ages", done, g_n);
    }
    plat_mutex_unlock(&g_lock);
}

void dbg_any_age_items(int on) {
    atomic_store(&g_on, on != 0);
    if (cfg_bool("NX2NAVI_DEBUG_ANY_AGE_ITEMS") != (on != 0)) {
        cfg_set("NX2NAVI_DEBUG_ANY_AGE_ITEMS", on ? "1" : "0");
        menu_save_settings_soon();
    }
}

int dbg_pouch_entry_at(int i, uint64_t *at, uint8_t own[6]) {
    plat_mutex_lock(&g_lock);
    int ok = i >= 0 && i < g_n;
    if (ok) *at = g_entries[i].at, memcpy(own, g_entries[i].own, 6);
    plat_mutex_unlock(&g_lock);
    return ok;
}

int dbg_pouch_entries(void) {
    plat_mutex_lock(&g_lock);
    int n = g_n;
    plat_mutex_unlock(&g_lock);
    return n;
}
