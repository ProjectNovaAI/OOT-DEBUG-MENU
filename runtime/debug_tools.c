// Debug tools of the in-game menu (debug_tools.h). Game build 1.1.1 only: the player actor is found the way
// frame_rate.c finds it ([[main+0x4e371d0] + 0x480], name "Player" at +0x20, position floats at +0x31c).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "debug_remote.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define PLAYER_MANAGER_SLOT 0x4e371d0   // main+: pointer to the object that holds the player actor
#define PLAYER_ACTOR_FIELD 0x480        // in that object
#define ACTOR_NAME_FIELD 0x20
#define ACTOR_POS_FIELD 0x31c
#define SPEED_WINDOW_NS 100000000ull    // the speed is the displacement over about this long
#define TELEPORT_JUMP_M 20.0f

int dbg_available(void) { return RT_V111; }

static int guest_read(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }

static uint64_t guest_ptr(uint64_t a) {
    uint64_t p = 0;
    return guest_read(a, &p, 8) ? p : 0;
}

static uint64_t player_actor(void) {
    if (!dbg_available()) return 0;
    uint64_t manager = guest_ptr(MOD_MAIN_BASE + PLAYER_MANAGER_SLOT);
    uint64_t actor = manager ? guest_ptr(manager + PLAYER_ACTOR_FIELD) : 0;
    char name[7];
    uint64_t name_ptr = actor ? guest_ptr(actor + ACTOR_NAME_FIELD) : 0;
    if (!name_ptr || !guest_read(name_ptr, name, sizeof name) || memcmp(name, "Player", sizeof name)) return 0;
    return actor;
}

uint64_t dbg_player_actor(void) { return player_actor(); }

int dbg_player_pos(float pos[3]) {
    uint64_t actor = player_actor();
    if (!actor || !guest_read(actor + ACTOR_POS_FIELD, pos, 12)) return 0;
    return isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]);
}

int dbg_player_set_pos(const float pos[3]) {
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return 0;
    if (dbg_player_teleport(pos)) return 1;
    uint64_t actor = player_actor();
    if (!actor) return 0;
    uint64_t at = actor + ACTOR_POS_FIELD, end;
    if (plat_mem_state(at & ~0xFFFull, (at & ~0xFFFull) + 0x2000, &end) != PLAT_PROT_RW) return 0;   // the heap: read-write
    plat_mem_prepare_write(at, 12);
    memcpy((void *)(uintptr_t)at, pos, 12);
    return 1;
}

// ---- health

#define HEALTH_HASH 0x31AB5580u
#define HEALTH_MAX_HASH 0xFBE01DA1u
#define HEALTH_SCAN_FROM 0x100000000ull   // the guest heap lies between these (host addresses = guest addresses)
#define HEALTH_SCAN_TO 0x700000000ull

static _Atomic uint64_t health_addr;
static _Atomic int health_locating, god_on;

static int health_entry_ok(uint64_t a, uint32_t out[8]) {
    uint32_t e[8];
    if (!a || !guest_read(a, e, sizeof e)) return 0;
    uint64_t p0 = e[2] | (uint64_t)e[3] << 32, p1 = e[6] | (uint64_t)e[7] << 32;
    if (e[1] != HEALTH_HASH || e[5] != HEALTH_MAX_HASH) return 0;
    if (p0 < MOD_MAIN_BASE || p0 >= MOD_MAIN_BASE + 0x10000000ull || p1 < MOD_MAIN_BASE || p1 >= MOD_MAIN_BASE + 0x10000000ull) return 0;
    if (e[0] > 400 || e[4] < 4 || e[4] > 400) return 0;
    if (out) memcpy(out, e, sizeof e);
    return 1;
}

static void health_locate_thread(void *arg) {
    (void)arg;
    enum { CHUNK = 1 << 20 };
    static uint32_t buf[CHUNK / 4];
    uint64_t addr = HEALTH_SCAN_FROM;
    while (addr < HEALTH_SCAN_TO && !atomic_load(&health_addr)) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, HEALTH_SCAN_TO, &end);
        if (end <= addr) break;
        if (st == PLAT_PROT_RW) {
            for (uint64_t at = addr; at < end; at += CHUNK) {
                uint64_t n = (end - at < CHUNK ? end - at : CHUNK) / 4;
                if (!plat_mem_read(at, buf, n * 4)) continue;
                for (uint64_t i = 1; i < n; i++)
                    if (buf[i] == HEALTH_HASH && health_entry_ok(at + (i - 1) * 4, NULL)) {
                        atomic_store(&health_addr, at + (i - 1) * 4);
                        break;
                    }
                if (atomic_load(&health_addr)) break;
            }
        }
        addr = end;
    }
    atomic_store(&health_locating, 0);
}

int dbg_health_locating(void) { return atomic_load(&health_locating); }

int dbg_health_get(int *cur, int *max) {
    if (!dbg_available()) return 0;
    uint32_t e[8];
    uint64_t a = atomic_load(&health_addr);
    if (a && health_entry_ok(a, e)) {
        if (cur) *cur = (int)e[0];
        if (max) *max = (int)e[4];
        return 1;
    }
    if (a) atomic_store(&health_addr, 0);   // moved (a new save was loaded): look again
    int zero = 0;
    if (atomic_compare_exchange_strong(&health_locating, &zero, 1)) {
        if (!plat_thread_spawn(health_locate_thread, NULL, "debug-health", 0)) atomic_store(&health_locating, 0);
    }
    return 0;
}

static int health_write(uint64_t at, uint32_t v) {
    uint64_t end;
    if (plat_mem_state(at & ~0xFFFull, (at & ~0xFFFull) + 0x2000, &end) != PLAT_PROT_RW) return 0;
    plat_mem_prepare_write(at, 4);
    memcpy((void *)(uintptr_t)at, &v, 4);
    return 1;
}

int dbg_health_set(int cur) {
    int c, m;
    if (!dbg_health_get(&c, &m)) return 0;
    if (cur < 0) cur = 0;
    if (cur > m) cur = m;
    return health_write(atomic_load(&health_addr), (uint32_t)cur);
}

int dbg_health_set_max(int max) {
    int c, m;
    if (!dbg_health_get(&c, &m)) return 0;
    if (max < 4) max = 4;
    if (max > 80) max = 80;
    return health_write(atomic_load(&health_addr) + 0x10, (uint32_t)max);
}

void dbg_invulnerable(int on) { atomic_store(&god_on, on != 0); }
int dbg_invulnerable_on(void) { return atomic_load(&god_on); }

static _Atomic int lock_val;

void dbg_health_lock(int quarters) { atomic_store(&lock_val, quarters < 0 ? 0 : quarters > 80 ? 80 : quarters); }
int dbg_health_lock_value(void) { return atomic_load(&lock_val); }

static void health_tick(void) {
    int lock = atomic_load(&lock_val);
    if (!lock && !atomic_load(&god_on)) return;
    int c, m;
    if (!dbg_health_get(&c, &m)) return;
    int want = lock ? (lock < m ? lock : m) : m;
    if (c != want) health_write(atomic_load(&health_addr), (uint32_t)want);
}

// ---- flags, magic and counted items

// The flag stores, one heap array per value type. Every entry starts with a pointer to its type's class in main; the
// value sits at `value` (int, float and enum entries keep it again 4 bytes later). The array's {key, index} hash table,
// about two slots per entry with empty slots zero, ends right where the entries start.
static const struct StoreInfo {
    uint32_t cls, size, value, min;   // class (main+), entry size, value offset, fewest entries a real store has
} store_info[DBG_FLAG_TYPES] = {
    [DBG_FLAG_BOOL] = {0x4c20658, 0x18, 0x16, 4000},
    [DBG_FLAG_INT] = {0x4c20718, 0x28, 0x1c, 500},
    [DBG_FLAG_FLOAT] = {0x4c207d8, 0x28, 0x1c, 40},
    [DBG_FLAG_STRING] = {0x4c20c58, 0x108, 0x74, 30},
    [DBG_FLAG_VEC3] = {0x4c20a18, 0x40, 0x28, 10},
    [DBG_FLAG_ENUM] = {0x4c20898, 0x38, 0x1c, 60},
};
#define STORE_MAX 32768                 // more entries than any store has
#define STORE_NEAR 0x1000000ull         // the other stores lie within this distance of the int store
#define MAGIC_HASH 0x76B18200u          // PlayerStatus.MP
#define MAGIC_MAX_HASH 0x12D63A97u      // PlayerStatus.MPMax
#define MAGIC_LIMIT 2000

static struct Store {
    _Atomic uint64_t base;
    int count;
    uint32_t *keys;    // a copy of the hash table (pairs), read when the store is found
    size_t npairs;
} stores[DBG_FLAG_TYPES];
static _Atomic uint64_t magic_addr;
static _Atomic int flags_locating;

// MurmurHash3 (32 bit, seed 0) of a flag's name: the key the game stores the flag under.
uint32_t dbg_flag_key(const char *name) {
    size_t n = strlen(name), i;
    uint32_t h = 0, k;
    for (i = 0; i + 4 <= n; i += 4) {
        memcpy(&k, name + i, 4);
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    for (size_t j = n; j > i; j--) k = k << 8 | (uint8_t)name[j - 1];
    if (n > i) k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    h ^= (uint32_t)n;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

// The index of a key in a store's copied table, -1 when it is not there. The table is searched from its end (next to the
// entries) back, so a pair of the table before it (whose index may also look valid) is never taken first.
static int store_index(const struct Store *s, uint32_t key) {
    if (!s->keys || !key) return -1;
    for (size_t i = s->npairs; i-- > 0;)
        if (s->keys[i * 2] == key && s->keys[i * 2 + 1] < (uint32_t)s->count) return (int)s->keys[i * 2 + 1];
    return -1;
}

static int store_count(int t, uint64_t base) {
    const uint64_t cls = MOD_MAIN_BASE + store_info[t].cls;
    int n = 0;
    while (n < STORE_MAX && guest_ptr(base + (uint64_t)n * store_info[t].size) == cls) n++;
    return n;
}

// Takes the run of entries at `base` as store t: copies its hash table (two slots per entry and some spare). The int
// store is accepted only when its table knows the maximum health and that entry holds a plausible maximum.
static int store_take(int t, uint64_t base, int n) {
    struct Store *s = &stores[t];
    size_t npairs = (size_t)n * 2 + 64;
    uint32_t *keys = s->keys && s->npairs >= npairs ? s->keys : realloc(s->keys, npairs * 8);
    if (!keys) return 0;
    s->keys = keys;
    if (!guest_read(base - npairs * 8, keys, npairs * 8)) return 0;
    s->npairs = npairs;
    s->count = n;
    if (t == DBG_FLAG_INT) {
        int i = store_index(s, HEALTH_MAX_HASH);
        uint32_t v = 0;
        if (i < 0 || !guest_read(base + (uint64_t)i * store_info[t].size + store_info[t].value, &v, 4) || v < 4 || v > 400) return 0;
    }
    atomic_store(&s->base, base);
    return 1;
}

// One pass over the heap finds the int store (the run of entries whose table knows the maximum health), the start of
// every run of the other stores' entries and the magic meter of the player, which keeps the two magic keys right before
// its value. The other stores are then taken from the runs nearest to the int store: the game allocates them together.
static void flags_locate_thread(void *arg) {
    (void)arg;
    enum { CHUNK = 1 << 20, OVER = 2, CAND = 16 };
    static uint64_t buf[CHUNK / 8 + OVER];
    static uint64_t cand[DBG_FLAG_TYPES][CAND];
    int ncand[DBG_FLAG_TYPES] = {0};
    uint64_t addr = HEALTH_SCAN_FROM;
    while (addr < HEALTH_SCAN_TO) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, HEALTH_SCAN_TO, &end);
        if (end <= addr) break;
        if (st == PLAT_PROT_RW) {
            for (uint64_t at = addr; at < end; at += CHUNK) {
                uint64_t n = (end - at < CHUNK ? end - at : CHUNK) / 8;
                uint64_t rn = end - at >= (n + OVER) * 8 ? n + OVER : n;
                if (!plat_mem_read(at, buf, rn * 8)) continue;
                for (uint64_t i = 0; i < n; i++) {
                    uint64_t v = buf[i];
                    if (v - (MOD_MAIN_BASE + 0x4c20600ull) < 0x900ull) {
                        for (int t = 0; t < DBG_FLAG_TYPES; t++) {
                            uint64_t cls = MOD_MAIN_BASE + store_info[t].cls, e = at + i * 8;
                            if (v != cls || ncand[t] >= CAND || guest_ptr(e - store_info[t].size) == cls) continue;
                            if (guest_ptr(e + store_info[t].size) != cls) continue;
                            cand[t][ncand[t]++] = e;
                        }
                    }
                    const uint32_t *w = (const uint32_t *)&buf[i];   // the meter is 4-byte aligned: both halves
                    for (int h = 0; h < 2 && i + 2 < rn; h++)
                        if (w[h] == MAGIC_HASH && w[h + 1] == MAGIC_MAX_HASH && w[h + 3] <= MAGIC_LIMIT && !atomic_load(&magic_addr))
                            atomic_store(&magic_addr, at + i * 8 + (uint64_t)(h + 3) * 4);
                }
            }
        }
        addr = end;
    }
    uint64_t int_base = 0;
    for (int k = 0; k < ncand[DBG_FLAG_INT] && !int_base; k++) {
        int n = store_count(DBG_FLAG_INT, cand[DBG_FLAG_INT][k]);
        if (n >= (int)store_info[DBG_FLAG_INT].min && store_take(DBG_FLAG_INT, cand[DBG_FLAG_INT][k], n)) int_base = cand[DBG_FLAG_INT][k];
    }
    for (int t = 0; t < DBG_FLAG_TYPES && int_base; t++) {
        if (t == DBG_FLAG_INT) continue;
        uint64_t best = 0, best_d = STORE_NEAR;
        int best_n = 0;
        for (int k = 0; k < ncand[t]; k++) {
            uint64_t d = cand[t][k] > int_base ? cand[t][k] - int_base : int_base - cand[t][k];
            int n = store_count(t, cand[t][k]);
            if (d < best_d && n >= (int)store_info[t].min) best = cand[t][k], best_d = d, best_n = n;
        }
        if (best) store_take(t, best, best_n);
    }
    atomic_store(&flags_locating, 0);
}

static void flags_locate(void) {
    int zero = 0;
    if (!dbg_available() || !atomic_compare_exchange_strong(&flags_locating, &zero, 1)) return;
    if (!plat_thread_spawn(flags_locate_thread, NULL, "debug-flags", 0)) atomic_store(&flags_locating, 0);
}

int dbg_flags_locating(void) { return atomic_load(&flags_locating); }

// A first entry that no longer starts with its class pointer means the stores moved (the game went back to the title
// and loaded again): all of them are looked for again.
static int stores_ready(void) {
    uint64_t base = atomic_load(&stores[DBG_FLAG_INT].base);
    if (base && guest_ptr(base) == MOD_MAIN_BASE + store_info[DBG_FLAG_INT].cls) return 1;
    if (base)
        for (int t = 0; t < DBG_FLAG_TYPES; t++) atomic_store(&stores[t].base, 0);
    flags_locate();
    return 0;
}

uint64_t dbg_flag_addr(int type, uint32_t key) {
    if (type < 0 || type >= DBG_FLAG_TYPES || !stores_ready()) return 0;
    struct Store *s = &stores[type];
    uint64_t base = atomic_load(&s->base);
    int i = base ? store_index(s, key) : -1;
    return i < 0 ? 0 : base + (uint64_t)i * store_info[type].size + store_info[type].value;
}

int dbg_flag_type(uint32_t key) {
    for (int t = 0; t < DBG_FLAG_TYPES; t++)
        if (dbg_flag_addr(t, key)) return t;
    return -1;
}

int dbg_flag_get(uint32_t key, int *value) {
    uint64_t a = dbg_flag_addr(DBG_FLAG_INT, key);
    uint32_t v = 0;
    if (!a || !guest_read(a, &v, 4)) return 0;
    if (value) *value = (int)v;
    return 1;
}

static int guest_write(uint64_t at, const void *src, size_t n) {
    uint64_t end;
    if (!at || plat_mem_state(at & ~0xFFFull, ((at + n) & ~0xFFFull) + 0x1000, &end) != PLAT_PROT_RW) return 0;
    plat_mem_prepare_write(at, n);
    memcpy((void *)(uintptr_t)at, src, n);
    return 1;
}

int dbg_flag_set(uint32_t key, int value) {
    uint64_t a = dbg_flag_addr(DBG_FLAG_INT, key);
    return a && health_write(a, (uint32_t)value) && health_write(a + 4, (uint32_t)value);
}

int dbg_flag_get_bool(uint32_t key) {
    uint8_t v = 0;
    uint64_t a = dbg_flag_addr(DBG_FLAG_BOOL, key);
    return a && guest_read(a, &v, 1) ? v != 0 : -1;
}

int dbg_flag_set_bool(uint32_t key, int on) {
    uint8_t v = on != 0;
    return guest_write(dbg_flag_addr(DBG_FLAG_BOOL, key), &v, 1);
}

int dbg_flag_get_u32(int type, uint32_t key, uint32_t *value) {
    uint64_t a = dbg_flag_addr(type, key);
    return a && guest_read(a, value, 4);
}

int dbg_flag_set_u32(int type, uint32_t key, uint32_t value) { return guest_write(dbg_flag_addr(type, key), &value, 4); }

int dbg_flag_get_string(uint32_t key, char out[64]) {
    uint64_t a = dbg_flag_addr(DBG_FLAG_STRING, key);
    if (!a || !guest_read(a, out, 64)) return 0;
    out[63] = 0;
    return 1;
}

int dbg_flag_set_string(uint32_t key, const char *text) {
    char v[64] = {0};
    strncpy(v, text, sizeof v - 1);
    return guest_write(dbg_flag_addr(DBG_FLAG_STRING, key), v, strlen(v) + 1);
}

int dbg_flag_get_vec3(uint32_t key, float v[3]) {
    uint64_t a = dbg_flag_addr(DBG_FLAG_VEC3, key);
    return a && guest_read(a, v, 12);
}

int dbg_flag_set_vec3(uint32_t key, const float v[3]) { return guest_write(dbg_flag_addr(DBG_FLAG_VEC3, key), v, 12); }

// The magic meter: the player's own copy is the one the game uses (it writes it over the flag on every frame), so that is
// what is read and written. The maximum is only a flag.
int dbg_magic_get(int *cur, int *max) {
    uint64_t a = atomic_load(&magic_addr);
    uint32_t e[4];
    if (a && (!guest_read(a - 12, e, sizeof e) || e[0] != MAGIC_HASH || e[1] != MAGIC_MAX_HASH || e[3] > MAGIC_LIMIT))
        atomic_store(&magic_addr, 0), a = 0;
    if (!a) {
        flags_locate();
        return 0;
    }
    int m = 0;
    if (cur) *cur = (int)e[3];
    if (max) *max = dbg_flag_get(MAGIC_MAX_HASH, &m) ? m : 0;
    return 1;
}

int dbg_magic_set(int value) {
    int c, m;
    if (!dbg_magic_get(&c, &m)) return 0;
    if (value < 0) value = 0;
    if (m > 0 && value > m) value = m;
    return health_write(atomic_load(&magic_addr), (uint32_t)value);
}

static _Atomic int magic_hold;

void dbg_magic_infinite(int on) { atomic_store(&magic_hold, on != 0); }
int dbg_magic_infinite_on(void) { return atomic_load(&magic_hold); }

static void magic_tick(void) {
    int c, m;
    if (atomic_load(&magic_hold) && dbg_magic_get(&c, &m) && m > 0 && c != m) dbg_magic_set(m);
}

// The counted things the menu can change. `flag` is the game's name of the int flag; `key` is filled in on first use.
// The first ITEMS_CONSUMED entries are the ones that get used up: the infinite items switch holds those at their largest value.
#define ITEMS_CONSUMED 8
static struct Item {
    const char *label, *flag;
    int max, step;
    uint32_t key;
} items[] = {
    {"Rupees", "PlayerStatus.CurrentRupee", 999, 10, 0},
    {"Arrows", "PouchData.Item_Arrow_Normal.StockNum", 50, 5, 0},
    {"Bombs", "PouchData.Item_Bomb.StockNum", 40, 5, 0},
    {"Bombchus", "PouchData.Item_Bombchu.StockNum", 50, 5, 0},
    {"Deku sticks", "PouchData.Item_DekuStick.StockNum", 30, 5, 0},
    {"Deku nuts", "PouchData.Item_DekuNut.StockNum", 40, 5, 0},
    {"Deku seeds", "PouchData.Item_DekuSeed.StockNum", 50, 5, 0},
    {"Magic beans", "PouchData.Item_MagicBean.StockNum", 10, 1, 0},
    {"Gold Skulltula tokens", "PouchData.Item_Token_Gold.StockNum", 100, 5, 0},
    {"Small keys: Forest Temple", "DungeonInfo.ForestTemple.SmallKeyNum", 9, 1, 0},
    {"Small keys: Fire Temple", "DungeonInfo.FireTemple.SmallKeyNum", 9, 1, 0},
    {"Small keys: Water Temple", "DungeonInfo.WaterTemple.SmallKeyNum", 9, 1, 0},
    {"Small keys: Spirit Temple", "DungeonInfo.SpiritTemple.SmallKeyNum", 9, 1, 0},
    {"Small keys: Shadow Temple", "DungeonInfo.ShadowTemple.SmallKeyNum", 9, 1, 0},
    {"Small keys: Bottom of the Well", "DungeonInfo.BottomOfTheWell.SmallKeyNum", 9, 1, 0},
    {"Small keys: Gerudo Training Ground", "DungeonInfo.GerudoTrainingGround.SmallKeyNum", 9, 1, 0},
    {"Small keys: Ganon's Castle", "DungeonInfo.InsideGanonCastle.SmallKeyNum", 9, 1, 0},
};

int dbg_item_count(void) { return (int)(sizeof items / sizeof items[0]); }

const char *dbg_item_label(int i, int *max, int *step) {
    if (i < 0 || i >= dbg_item_count()) return NULL;
    if (max) *max = items[i].max;
    if (step) *step = items[i].step;
    return items[i].label;
}

static uint32_t item_key(int i) {
    if (!items[i].key) items[i].key = dbg_flag_key(items[i].flag);
    return items[i].key;
}

int dbg_item_get(int i, int *value) { return i >= 0 && i < dbg_item_count() && dbg_flag_get(item_key(i), value); }

int dbg_item_set(int i, int value) {
    if (i < 0 || i >= dbg_item_count()) return 0;
    if (value < 0) value = 0;
    if (value > items[i].max) value = items[i].max;
    return dbg_flag_set(item_key(i), value);
}

static _Atomic int items_hold;

void dbg_items_infinite(int on) { atomic_store(&items_hold, on != 0); }
int dbg_items_infinite_on(void) { return atomic_load(&items_hold); }

static void items_tick(void) {
    if (!atomic_load(&items_hold)) return;
    for (int i = 0, v; i < ITEMS_CONSUMED; i++)
        if (dbg_item_get(i, &v) && v != items[i].max) dbg_item_set(i, items[i].max);
}

// Owned things. The game keeps an item in two flags: PouchData.<name>.StockNum (1 for an item that is not counted; the
// count for ammunition) and IsGet.<name> (bool: ever got, which also skips the first-get scene). Songs have the bool
// only. Giving sets both; removing sets the count to 0 and the bool off. Tested: the pouch item appears and disappears
// in the menus; double magic (below) doubles the meter on screen at once.
static struct Owned {
    const char *group, *label, *name;
    int pouch;   // has a PouchData count
    uint32_t stock_key, get_key;
} owned[] = {
    {"Equipment", "Kokiri Sword", "Weapon_Sword_Kokiri", 1},
    {"Equipment", "Master Sword", "Weapon_Sword_Master", 1},
    {"Equipment", "Biggoron's Sword", "Weapon_Sword_Giant", 1},
    {"Equipment", "Deku Shield", "Weapon_Shield_Deku", 1},
    {"Equipment", "Hylian Shield", "Weapon_Shield_Hylian", 1},
    {"Equipment", "Mirror Shield", "Weapon_Shield_Mirror", 1},
    {"Equipment", "Goron Tunic", "Armor_Adult_Goron", 1},
    {"Equipment", "Zora Tunic", "Armor_Adult_Zora", 1},
    {"Equipment", "Iron Boots", "Armor_Adult_HeavyBoot", 1},
    {"Equipment", "Hover Boots", "Armor_Adult_HoverBoot", 1},
    {"Items", "Fairy Slingshot", "Weapon_Range_SlingShot", 1},
    {"Items", "Fairy Bow", "Weapon_Range_Bow", 1},
    {"Items", "Fire Arrows", "Item_Arrow_Fire", 1},
    {"Items", "Ice Arrows", "Item_Arrow_Ice", 1},
    {"Items", "Light Arrows", "Item_Arrow_Light", 1},
    {"Items", "Boomerang", "Item_Boomerang", 1},
    {"Items", "Hookshot", "Item_Hookshot", 1},
    {"Items", "Longshot", "Item_Longshot", 1},
    {"Items", "Megaton Hammer", "Item_Hammer", 1},
    {"Items", "Lens of Truth", "Armor_GlassTruth", 1},
    {"Items", "Din's Fire", "Item_DinFire", 1},
    {"Items", "Farore's Wind", "Item_FaroreWind", 1},
    {"Items", "Nayru's Love", "Item_NayruLove", 1},
    {"Items", "Fairy Ocarina", "Item_Ocarina_Fairy", 1},
    {"Items", "Ocarina of Time", "Item_Ocarina_Time", 1},
    {"Upgrades", "Goron's Bracelet", "Armor_Child_Gauntlet_Goron", 1},
    {"Upgrades", "Silver Gauntlets", "Armor_Adult_Gauntlet_Silver", 1},
    {"Upgrades", "Golden Gauntlets", "Armor_Adult_Gauntlet_Gold", 1},
    {"Upgrades", "Silver Scale", "Item_Scale_Silver", 1},
    {"Upgrades", "Golden Scale", "Item_Scale_Gold", 1},
    {"Upgrades", "Bomb Bag", "Item_BombBag_Small", 1},
    {"Upgrades", "Big Bomb Bag", "Item_BombBag_Medium", 1},
    {"Upgrades", "Biggest Bomb Bag", "Item_BombBag_Large", 1},
    {"Upgrades", "Quiver", "Item_Quiver_Small", 1},
    {"Upgrades", "Big Quiver", "Item_Quiver_Medium", 1},
    {"Upgrades", "Biggest Quiver", "Item_Quiver_Large", 1},
    {"Upgrades", "Bullet Bag", "Item_BulletBag_Small", 1},
    {"Upgrades", "Bigger Bullet Bag", "Item_BulletBag_Medium", 1},
    {"Upgrades", "Biggest Bullet Bag", "Item_BulletBag_Large", 1},
    {"Upgrades", "Adult's Wallet", "Item_Wallet_Medium", 1},
    {"Upgrades", "Giant's Wallet", "Item_Wallet_Large", 1},
    {"Upgrades", "Stone of Agony", "Item_AgonyStone", 1},
    {"Upgrades", "Gerudo's Card", "Item_Token_Gerudo", 1},
    {"Quest", "Kokiri's Emerald", "Item_Stone_Forest", 1},
    {"Quest", "Goron's Ruby", "Item_Stone_Fire", 1},
    {"Quest", "Zora's Sapphire", "Item_Stone_Water", 1},
    {"Quest", "Forest Medallion", "Item_Medal_Forest", 1},
    {"Quest", "Fire Medallion", "Item_Medal_Fire", 1},
    {"Quest", "Water Medallion", "Item_Medal_Water", 1},
    {"Quest", "Spirit Medallion", "Item_Medal_Spirit", 1},
    {"Quest", "Shadow Medallion", "Item_Medal_Shadow", 1},
    {"Quest", "Light Medallion", "Item_Medal_Light", 1},
    {"Songs", "Zelda's Lullaby", "Item_Melody_ZeldaLullaby", 0},
    {"Songs", "Epona's Song", "Item_Melody_EponaSong", 0},
    {"Songs", "Saria's Song", "Item_Melody_SariaSong", 0},
    {"Songs", "Sun's Song", "Item_Melody_SunSong", 0},
    {"Songs", "Song of Time", "Item_Melody_TimeSong", 0},
    {"Songs", "Song of Storms", "Item_Melody_StormSong", 0},
    {"Songs", "Scarecrow's Song", "Item_Melody_ScarecrowSong", 0},
    {"Songs", "Minuet of Forest", "Item_Melody_ForestMinuet", 0},
    {"Songs", "Bolero of Fire", "Item_Melody_FireBolero", 0},
    {"Songs", "Serenade of Water", "Item_Melody_WaterSerenade", 0},
    {"Songs", "Requiem of Spirit", "Item_Melody_SpiritRequiem", 0},
    {"Songs", "Nocturne of Shadow", "Item_Melody_ShadowNocturne", 0},
    {"Songs", "Prelude of Light", "Item_Melody_LightPrelude", 0},
    {"Masks", "Keaton Mask", "Armor_Child_Mask_Keaton", 1},
    {"Masks", "Skull Mask", "Armor_Child_Mask_Skull", 1},
    {"Masks", "Spooky Mask", "Armor_Child_Mask_Spooky", 1},
    {"Masks", "Bunny Hood", "Armor_Child_Mask_Bunny", 1},
    {"Masks", "Goron Mask", "Armor_Child_Mask_Goron", 1},
    {"Masks", "Zora Mask", "Armor_Child_Mask_Zora", 1},
    {"Masks", "Gerudo Mask", "Armor_Child_Mask_Gerudo", 1},
    {"Masks", "Mask of Truth", "Armor_Child_Mask_Truth", 1},
};
#define NOWNED ((int)(sizeof owned / sizeof owned[0]))

static void owned_keys(struct Owned *o) {
    if (o->get_key) return;
    char name[96];
    snprintf(name, sizeof name, "PouchData.%s.StockNum", o->name);
    o->stock_key = o->pouch ? dbg_flag_key(name) : 0;
    snprintf(name, sizeof name, "IsGet.%s", o->name);
    o->get_key = dbg_flag_key(name);
}

int dbg_owned_count(void) { return NOWNED; }

const char *dbg_owned_label(int i, const char **group) {
    if (i < 0 || i >= NOWNED) return NULL;
    if (group) *group = owned[i].group;
    return owned[i].label;
}

int dbg_owned_get(int i) {
    if (i < 0 || i >= NOWNED) return -1;
    owned_keys(&owned[i]);
    int v = 0;
    if (owned[i].pouch) return dbg_flag_get(owned[i].stock_key, &v) ? v > 0 : -1;
    return dbg_flag_get_bool(owned[i].get_key);
}

int dbg_owned_set(int i, int on) {
    if (i < 0 || i >= NOWNED) return 0;
    owned_keys(&owned[i]);
    int v = 0, ok = 1;
    if (owned[i].pouch && dbg_flag_get(owned[i].stock_key, &v) && (on ? v <= 0 : v != 0))
        ok = dbg_flag_set(owned[i].stock_key, on ? 1 : 0);
    return dbg_flag_set_bool(owned[i].get_key, on) && ok;
}

// Magic level: the meter's maximum is PlayerStatus.MPMax (240 a level), and the two fairy blessings record the levels
// got. The current magic is kept within the new maximum.
#define MAGIC_LEVEL_UNITS 240
static const char *const magic_items[2] = {"Item_FairyBless_MP", "Item_FairyBless_MPDouble"};

int dbg_magic_level(void) {
    int m = 0;
    if (!dbg_flag_get(MAGIC_MAX_HASH, &m)) return -1;
    return m <= 0 ? 0 : m <= MAGIC_LEVEL_UNITS ? 1 : 2;
}

int dbg_magic_set_level(int level) {
    if (level < 0 || level > 2) return 0;
    char name[96];
    for (int k = 0; k < 2; k++) {
        snprintf(name, sizeof name, "PouchData.%s.StockNum", magic_items[k]);
        dbg_flag_set(dbg_flag_key(name), level > k);
        snprintf(name, sizeof name, "IsGet.%s", magic_items[k]);
        dbg_flag_set_bool(dbg_flag_key(name), level > k);
    }
    if (!dbg_flag_set(MAGIC_MAX_HASH, level * MAGIC_LEVEL_UNITS)) return 0;
    int c;
    if (dbg_magic_get(&c, NULL) && c > level * MAGIC_LEVEL_UNITS) dbg_magic_set(level * MAGIC_LEVEL_UNITS);
    return 1;
}

// ---- speed

static float last_pos[3];
static uint64_t last_ns, window_ns;
static float window_from[3];
static int have_last;
static _Atomic float ground_speed, vertical_speed;

void dbg_tick(uint64_t now_ns) {
    static int remote_checked;
    if (!remote_checked) {   // NX2NAVI_DEBUG_REMOTE=1 (environment or nx2navi.ini) starts the remote console without the menu
        remote_checked = 1;
        char v[8];
        if (plat_getenv("NX2NAVI_DEBUG_REMOTE", v, sizeof v) && v[0] && v[0] != '0') dbg_remote_set(1);
    }
    health_tick();
    magic_tick();
    items_tick();
    float pos[3];
    if (!dbg_player_pos(pos)) {
        have_last = 0;
        atomic_store(&ground_speed, 0.0f);
        atomic_store(&vertical_speed, 0.0f);
        return;
    }
    float dx = pos[0] - last_pos[0], dy = pos[1] - last_pos[1], dz = pos[2] - last_pos[2];
    if (!have_last || sqrtf(dx * dx + dy * dy + dz * dz) > TELEPORT_JUMP_M) {   // first sight or a jump: start over
        window_ns = now_ns;
        memcpy(window_from, pos, sizeof pos);
        atomic_store(&ground_speed, 0.0f);
        atomic_store(&vertical_speed, 0.0f);
    } else if (now_ns - window_ns >= SPEED_WINDOW_NS) {
        double dt = (double)(now_ns - window_ns) * 1e-9;
        float wx = pos[0] - window_from[0], wy = pos[1] - window_from[1], wz = pos[2] - window_from[2];
        atomic_store(&ground_speed, (float)(sqrt((double)wx * wx + (double)wz * wz) / dt));
        atomic_store(&vertical_speed, (float)(wy / dt));
        window_ns = now_ns;
        memcpy(window_from, pos, sizeof pos);
    }
    memcpy(last_pos, pos, sizeof pos);
    last_ns = now_ns;
    have_last = 1;
}

float dbg_ground_speed(void) { return atomic_load(&ground_speed); }
float dbg_vertical_speed(void) { return atomic_load(&vertical_speed); }

// ---- saved positions

static float slot_pos[DBG_SLOTS][3];
static int slot_set[DBG_SLOTS];

int dbg_slot_save(int slot) {
    if (slot < 0 || slot >= DBG_SLOTS) return 0;
    float pos[3];
    if (!dbg_player_pos(pos)) return 0;
    memcpy(slot_pos[slot], pos, sizeof pos);
    slot_set[slot] = 1;
    return 1;
}

int dbg_slot_load(int slot) {
    if (slot < 0 || slot >= DBG_SLOTS || !slot_set[slot]) return 0;
    return dbg_player_set_pos(slot_pos[slot]);
}

int dbg_slot_used(int slot, float pos[3]) {
    if (slot < 0 || slot >= DBG_SLOTS || !slot_set[slot]) return 0;
    if (pos) memcpy(pos, slot_pos[slot], 12);
    return 1;
}

// ---- game speed

static _Atomic float time_scale = 1.0f;

float dbg_time_scale(void) { return atomic_load_explicit(&time_scale, memory_order_relaxed); }

void dbg_set_time_scale(float scale) {
    if (!isfinite(scale)) scale = 1.0f;
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 8.0f) scale = 8.0f;
    atomic_store_explicit(&time_scale, scale, memory_order_relaxed);
}

// ---- timer

static int timer_on;
static uint64_t timer_start_ns, timer_acc_ns;
static uint64_t timer_start_ticks, timer_acc_ticks;
static uint64_t lap_ns[DBG_LAPS];
static int lap_n;

int dbg_timer_running(void) { return timer_on; }

uint64_t dbg_timer_ns(void) { return timer_acc_ns + (timer_on ? plat_now_ns() - timer_start_ns : 0); }

uint64_t dbg_timer_ticks(void) { return timer_acc_ticks + (timer_on ? frame_step_ticks() - timer_start_ticks : 0); }

void dbg_timer_toggle(void) {
    if (timer_on) {
        timer_acc_ns += plat_now_ns() - timer_start_ns;
        timer_acc_ticks += frame_step_ticks() - timer_start_ticks;
        timer_on = 0;
    } else {
        timer_start_ns = plat_now_ns();
        timer_start_ticks = frame_step_ticks();
        timer_on = 1;
    }
}

void dbg_timer_reset(void) {
    timer_on = 0;
    timer_acc_ns = timer_acc_ticks = 0;
    lap_n = 0;
}

void dbg_timer_lap(void) {
    if (lap_n < DBG_LAPS) lap_ns[lap_n++] = dbg_timer_ns();
}

int dbg_timer_laps(uint64_t ns[DBG_LAPS]) {
    memcpy(ns, lap_ns, sizeof lap_ns);
    return lap_n;
}

// ---- overlay

static _Atomic int overlay_mask;

int dbg_overlay_mask(void) { return atomic_load_explicit(&overlay_mask, memory_order_relaxed); }

void dbg_overlay_set(int mask) { atomic_store_explicit(&overlay_mask, mask, memory_order_relaxed); }

// ---- hotkeys

static _Atomic int hotkeys_on;
static char toast_text[64];
static uint64_t toast_until;

int dbg_hotkeys(void) { return atomic_load_explicit(&hotkeys_on, memory_order_relaxed); }
void dbg_hotkeys_set(int on) { atomic_store_explicit(&hotkeys_on, on != 0, memory_order_relaxed); }

static void toast(const char *s) {
    strncpy(toast_text, s, sizeof toast_text - 1);
    toast_text[sizeof toast_text - 1] = 0;
    toast_until = plat_now_ns() + 2000000000ull;
}

const char *dbg_toast(uint64_t now_ns) { return now_ns < toast_until ? toast_text : NULL; }

int dbg_hotkey(int fkey) {
    static const float scales[] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    if (!dbg_hotkeys() || !dbg_available() || fkey < 5 || fkey > 9) return 0;
    char t[64];
    switch (fkey) {
    case 5: toast(dbg_slot_save(0) ? "Position saved to slot 1" : "No player to save"); break;
    case 6: toast(dbg_slot_load(0) ? "Moved to slot 1" : "Slot 1 is empty or no player"); break;
    case 7:
    case 8: {
        float s = dbg_time_scale();
        int idx = 0, n = (int)(sizeof scales / sizeof *scales);
        for (int i = 1; i < n; i++)
            if (fabsf(scales[i] - s) < fabsf(scales[idx] - s)) idx = i;
        idx += fkey == 8 ? 1 : -1;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        dbg_set_time_scale(scales[idx]);
        snprintf(t, sizeof t, "Game speed %.3gx", dbg_time_scale());
        toast(t);
        break;
    }
    default:
        dbg_timer_toggle();
        toast(dbg_timer_running() ? "Timer started" : "Timer stopped");
        break;
    }
    return 1;
}
