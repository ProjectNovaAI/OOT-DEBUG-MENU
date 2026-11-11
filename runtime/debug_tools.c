// Debug tools of the in-game menu (debug_tools.h). Game build 1.1.1 only: the player actor is found the way
// frame_rate.c finds it ([[main+0x4e371d0] + 0x480], name "Player" at +0x20, position floats at +0x31c).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
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

int dbg_player_pos(float pos[3]) {
    uint64_t actor = player_actor();
    if (!actor || !guest_read(actor + ACTOR_POS_FIELD, pos, 12)) return 0;
    return isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]);
}

int dbg_player_set_pos(const float pos[3]) {
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return 0;
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

// ---- int flags, magic and counted items

#define FLAG_ENTRY_CLASS 0x4c20718ull   // main+: the first 8 bytes of every int flag entry
#define FLAG_ENTRY_SIZE 0x28
#define FLAG_ENTRY_VALUE 0x1c           // the value, and the same value again 4 bytes later
#define FLAG_ENTRY_MAX 8192
#define FLAG_KEYS_SPAN 0x8000           // the {key, index} table lies in this many bytes before the first entry
#define MAGIC_HASH 0x76B18200u          // PlayerStatus.MP
#define MAGIC_MAX_HASH 0x12D63A97u      // PlayerStatus.MPMax
#define MAGIC_LIMIT 2000

static _Atomic uint64_t flag_base, magic_addr;
static _Atomic int flag_count, flags_locating;
static uint32_t flag_keys[FLAG_KEYS_SPAN / 4];   // a copy of the table, read once when the entries are found

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

// The index of a key in the copied table, -1 when it is not there. Entries are {key, index} pairs; empty slots are zero.
static int flag_index(uint32_t key) {
    int count = atomic_load(&flag_count);
    for (size_t i = 0; i + 1 < FLAG_KEYS_SPAN / 4; i += 2)
        if (flag_keys[i] == key && flag_keys[i + 1] < (uint32_t)count) return (int)flag_keys[i + 1];
    return -1;
}

// A run of entries starts at `base`: count it, copy the key table before it and accept it when the table knows the maximum
// health key and that entry holds a plausible maximum.
static int flags_try(uint64_t base) {
    const uint64_t cls = MOD_MAIN_BASE + FLAG_ENTRY_CLASS;
    int n = 0;
    while (n < FLAG_ENTRY_MAX && guest_ptr(base + (uint64_t)n * FLAG_ENTRY_SIZE) == cls) n++;
    if (n < 64 || !guest_read(base - FLAG_KEYS_SPAN, flag_keys, sizeof flag_keys)) return 0;
    atomic_store(&flag_count, n);
    int i = flag_index(HEALTH_MAX_HASH);
    uint32_t v = 0;
    if (i < 0 || !guest_read(base + (uint64_t)i * FLAG_ENTRY_SIZE + FLAG_ENTRY_VALUE, &v, 4) || v < 4 || v > 400) return 0;
    atomic_store(&flag_base, base);
    return 1;
}

// One pass over the heap finds both: the first entry of the int flag array (three entries in a row that start with the class
// pointer, none before them) and the magic meter of the player, which keeps the two magic keys right before its value.
static void flags_locate_thread(void *arg) {
    (void)arg;
    enum { CHUNK = 1 << 20, OVER = 32 };
    static uint32_t buf[CHUNK / 4 + OVER];
    const uint64_t cls = MOD_MAIN_BASE + FLAG_ENTRY_CLASS;
    const uint32_t lo = (uint32_t)cls, hi = (uint32_t)(cls >> 32);
    const int step = FLAG_ENTRY_SIZE / 4;
    uint64_t addr = HEALTH_SCAN_FROM;
    while (addr < HEALTH_SCAN_TO && !(atomic_load(&flag_base) && atomic_load(&magic_addr))) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, HEALTH_SCAN_TO, &end);
        if (end <= addr) break;
        if (st == PLAT_PROT_RW) {
            for (uint64_t at = addr; at < end; at += CHUNK) {
                uint64_t n = (end - at < CHUNK ? end - at : CHUNK) / 4;
                uint64_t rn = end - at >= (n + OVER) * 4 ? n + OVER : n;
                if (!plat_mem_read(at, buf, rn * 4)) continue;
                for (uint64_t i = 0; i < n && i + 2 * step + 1 < rn; i++) {
                    if (!(i & 1) && buf[i] == lo && buf[i + 1] == hi && buf[i + step] == lo && buf[i + step + 1] == hi &&
                        buf[i + 2 * step] == lo && buf[i + 2 * step + 1] == hi && !atomic_load(&flag_base) &&
                        guest_ptr(at + i * 4 - FLAG_ENTRY_SIZE) != cls)
                        flags_try(at + i * 4);
                    if (buf[i] == MAGIC_HASH && buf[i + 1] == MAGIC_MAX_HASH && buf[i + 3] <= MAGIC_LIMIT && !atomic_load(&magic_addr))
                        atomic_store(&magic_addr, at + (i + 3) * 4);
                }
            }
        }
        addr = end;
    }
    atomic_store(&flags_locating, 0);
}

static void flags_locate(void) {
    int zero = 0;
    if (!dbg_available() || !atomic_compare_exchange_strong(&flags_locating, &zero, 1)) return;
    if (!plat_thread_spawn(flags_locate_thread, NULL, "debug-flags", 0)) atomic_store(&flags_locating, 0);
}

int dbg_flags_locating(void) { return atomic_load(&flags_locating); }

// The address of a flag's value, 0 when the array or the key is not known. A first entry that no longer starts with the
// class pointer means the array moved (the game was restarted from the title): it is looked for again.
static uint64_t flag_value_addr(uint32_t key) {
    uint64_t base = atomic_load(&flag_base);
    if (base && guest_ptr(base) != MOD_MAIN_BASE + FLAG_ENTRY_CLASS) atomic_store(&flag_base, 0), base = 0;
    if (!base) {
        flags_locate();
        return 0;
    }
    int i = flag_index(key);
    return i < 0 ? 0 : base + (uint64_t)i * FLAG_ENTRY_SIZE + FLAG_ENTRY_VALUE;
}

int dbg_flag_get(uint32_t key, int *value) {
    uint64_t a = flag_value_addr(key);
    uint32_t v = 0;
    if (!a || !guest_read(a, &v, 4)) return 0;
    if (value) *value = (int)v;
    return 1;
}

int dbg_flag_set(uint32_t key, int value) {
    uint64_t a = flag_value_addr(key);
    return a && health_write(a, (uint32_t)value) && health_write(a + 4, (uint32_t)value);
}

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
