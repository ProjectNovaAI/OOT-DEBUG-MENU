// Save states of the debug menu (debug_tools.h "Save states"): the game's state as a save file would keep it, taken at
// any moment and put back at any time, also after a restart.
//
// A state holds the area and the age, the entrance the player last came in by, the player's position and facing,
// health and magic, and the value of every flag the game's own save file keeps (dbg_saved_flag_keys: the flags of the
// game's flag list whose SaveFileIndex is not -1, with one value and a 32-bit key, about 13,000): items, upgrades,
// rupees, quest progress, opened chests and doors, the time of day. The flags kept only while the game runs (the
// sequence, event and loading state among them) are left alone, and so is whatever the game holds outside its flags
// (live enemies, a lifted bomb), so a state is a place and a progress to go back to, not a freeze of every object.
//
// Loading: in the state's own area the flags are written and the player is moved at once. Elsewhere the state's
// entrance is warped to (debug_warp.c, as the area reload does) and the rest waits until the area has loaded and the
// player has stood there a moment (dbg_state_tick, once per presented frame). Health and magic go last: the player
// keeps its own copy of them and writes it over the flags.
//
// A state is kept in memory and in debug_state_<n>.bin in the game's folder: "NXST", u32 version, then the State head
// below, then its flags as {u32 key, u8 store (DBG_FLAG_), u8 length, the value's bytes}.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_tools.h"
#include "platform/platform.h"

#define STATE_MAGIC "NXST"
#define STATE_VERSION 1u
#define ARRIVE_WAIT_NS 30000000000ull   // a warp that has not arrived by then is given up
#define SETTLE_NS 1000000000ull          // how long the player stands in the arrived area before the state is put back

typedef struct {
    char banc[64], start[64];   // the area and the entrance last come in by ("" when none was seen)
    int32_t child;              // 1 child, 0 adult
    float pos[3], face[3];
    int32_t health, magic;      // -1 when unknown
    int64_t saved_at;           // seconds since 1970, for the menu
    uint32_t nflags;
} Head;

typedef struct {
    Head h;
    uint8_t *flags;   // nflags records
    size_t size;
    int set;
} State;

static State states[DBG_STATES];
static PlatMutex lock = PLAT_MUTEX_INIT;
static char note[160];
static uint32_t *saved_keys;
static int nsaved_keys = -1;

enum { L_IDLE, L_ARRIVE, L_SETTLE };
static int load_phase, load_slot;
static uint64_t load_since;

static void file_name(int slot, char out[32]) { snprintf(out, 32, "debug_state_%d.bin", slot + 1); }

static int is_sequence_key(uint32_t key) {
    static uint32_t skip[8];
    static int n = -1;
    if (n < 0) {
        static const char *const names[] = {"Sequence_CurrentBanc", "Sequence_IsChildhood", "Sequence_IsLoadingBanc",
                                            "Sequence_ReservedNextBanc", "Sequence_ReservedNextStartPosName",
                                            "Sequence_ControlInEvent", "Sequence_IsPause"};
        n = 0;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) skip[n++] = dbg_flag_key(names[i]);
    }
    for (int i = 0; i < n; i++)
        if (skip[i] == key) return 1;
    return 0;
}

// The value of one flag: its store and bytes (bool 1, int / float / enum 4, vec3 12, string up to 64). 0 when unknown.
static int flag_read(uint32_t key, int *type, uint8_t v[64], int *len) {
    int t = dbg_flag_type(key);
    *type = t;
    switch (t) {
    case DBG_FLAG_BOOL: {
        int b = dbg_flag_get_bool(key);
        if (b < 0) return 0;
        v[0] = (uint8_t)b, *len = 1;
        return 1;
    }
    case DBG_FLAG_INT:
    case DBG_FLAG_FLOAT:
    case DBG_FLAG_ENUM: {
        uint32_t u;
        if (!dbg_flag_get_u32(t, key, &u)) return 0;
        memcpy(v, &u, 4), *len = 4;
        return 1;
    }
    case DBG_FLAG_VEC3: {
        float f[3];
        if (!dbg_flag_get_vec3(key, f)) return 0;
        memcpy(v, f, 12), *len = 12;
        return 1;
    }
    case DBG_FLAG_STRING: {
        char s[64];
        if (!dbg_flag_get_string(key, s)) return 0;
        s[63] = 0;
        *len = (int)strlen(s);
        memcpy(v, s, (size_t)*len);
        return 1;
    }
    default: return 0;
    }
}

// Writes a flag back when it differs; 1 when it was written.
static int flag_write(uint32_t key, int type, const uint8_t *v, int len) {
    uint8_t now[64];
    int nt, nl;
    if (flag_read(key, &nt, now, &nl) && nt == type && nl == len && !memcmp(now, v, (size_t)len)) return 0;
    switch (type) {
    case DBG_FLAG_BOOL: return len == 1 && dbg_flag_set_bool(key, v[0] != 0);
    case DBG_FLAG_INT:
    case DBG_FLAG_FLOAT:
    case DBG_FLAG_ENUM: {
        uint32_t u;
        if (len != 4) return 0;
        memcpy(&u, v, 4);
        return dbg_flag_set_u32(type, key, u);
    }
    case DBG_FLAG_VEC3: {
        float f[3];
        if (len != 12) return 0;
        memcpy(f, v, 12);
        return dbg_flag_set_vec3(key, f);
    }
    case DBG_FLAG_STRING: {
        char s[64];
        if (len > 63) return 0;
        memcpy(s, v, (size_t)len), s[len] = 0;
        return dbg_flag_set_string(key, s);
    }
    default: return 0;
    }
}

static void state_free(State *s) {
    free(s->flags);
    memset(s, 0, sizeof *s);
}

static int state_write_file(int slot, const State *s) {
    char name[32];
    size_t n = 8 + sizeof s->h + s->size;
    uint8_t *buf = malloc(n);
    if (!buf) return 0;
    uint32_t ver = STATE_VERSION;
    memcpy(buf, STATE_MAGIC, 4), memcpy(buf + 4, &ver, 4), memcpy(buf + 8, &s->h, sizeof s->h);
    if (s->size) memcpy(buf + 8 + sizeof s->h, s->flags, s->size);
    file_name(slot, name);
    int ok = plat_file_write(name, buf, n) == 1;
    free(buf);
    return ok;
}

// The state of a slot from its file when it is not in memory yet (lock held).
static void state_read_file(int slot) {
    State *s = &states[slot];
    if (s->set) return;
    char name[32];
    uint64_t n = 0;
    file_name(slot, name);
    uint8_t *buf = plat_file_read(name, &n);
    uint32_t ver = 0;
    if (buf && n >= 8 + sizeof s->h && !memcmp(buf, STATE_MAGIC, 4) && (memcpy(&ver, buf + 4, 4), ver == STATE_VERSION)) {
        Head h;
        memcpy(&h, buf + 8, sizeof h);
        size_t rest = (size_t)n - 8 - sizeof h;
        uint8_t *f = rest ? malloc(rest) : NULL;
        if (!rest || f) {
            if (rest) memcpy(f, buf + 8 + sizeof h, rest);
            h.banc[63] = h.start[63] = 0;
            s->h = h, s->flags = f, s->size = rest, s->set = 1;
        }
    }
    free(buf);
}

int dbg_state_save(int slot) {
    if (slot < 0 || slot >= DBG_STATES) return 0;
    float pos[3];
    if (!dbg_player_pos(pos)) {
        snprintf(note, sizeof note, "No player to save");
        return 0;
    }
    uint64_t store_base;
    int store_count;
    uint32_t store_size, store_value;
    dbg_flag_type(dbg_flag_key("Sequence_CurrentBanc"));   // starts the search for the stores when none was made
    if (dbg_flags_locating() || !dbg_flag_store(DBG_FLAG_INT, &store_base, &store_count, &store_size, &store_value)) {
        snprintf(note, sizeof note, "The game's flags are still being found: try again in a moment");
        return 0;
    }
    plat_mutex_lock(&lock);
    if (nsaved_keys < 0) saved_keys = dbg_saved_flag_keys(&nsaved_keys);
    plat_mutex_unlock(&lock);
    if (nsaved_keys <= 0) {
        snprintf(note, sizeof note, "The game's flag list could not be read");
        return 0;
    }
    State s = {0};
    if (!dbg_current_area(s.h.banc)) s.h.banc[0] = 0;
    char lb[64], ls[64];
    if (dbg_last_entrance(lb, ls) && !strcmp(lb, s.h.banc)) memcpy(s.h.start, ls, 64);
    s.h.child = dbg_is_child() == 1;
    memcpy(s.h.pos, pos, sizeof pos);
    if (!dbg_player_facing(s.h.face)) s.h.face[0] = 0, s.h.face[1] = 0, s.h.face[2] = 1;
    int cur, max;
    s.h.health = dbg_health_get(&cur, &max) ? cur : -1;
    s.h.magic = dbg_magic_get(&cur, &max) ? cur : -1;
    s.h.saved_at = (int64_t)time(NULL);
    size_t cap = (size_t)nsaved_keys * 10;
    s.flags = malloc(cap);
    if (!s.flags) return 0;
    for (int i = 0; i < nsaved_keys; i++) {
        uint8_t v[64];
        int type, len;
        if (is_sequence_key(saved_keys[i]) || !flag_read(saved_keys[i], &type, v, &len)) continue;
        if (s.size + 6 + (size_t)len > cap) {
            uint8_t *g = realloc(s.flags, cap *= 2);
            if (!g) break;
            s.flags = g;
        }
        uint8_t *r = s.flags + s.size;
        memcpy(r, &saved_keys[i], 4), r[4] = (uint8_t)type, r[5] = (uint8_t)len, memcpy(r + 6, v, (size_t)len);
        s.size += 6 + (size_t)len;
        s.h.nflags++;
    }
    if (!s.h.nflags) {
        free(s.flags);
        snprintf(note, sizeof note, "The game's flags are still being found: try again in a moment");
        return 0;
    }
    s.set = 1;
    int written = state_write_file(slot, &s);
    plat_mutex_lock(&lock);
    state_free(&states[slot]);
    states[slot] = s;
    plat_mutex_unlock(&lock);
    snprintf(note, sizeof note, "State %d saved: %s, %u flags%s", slot + 1, s.h.banc[0] ? s.h.banc : "this area",
             s.h.nflags, written ? "" : " (not written to its file)");
    return 1;
}

// Flags, then the player's place, then health and magic (lock held).
static void apply(const State *s) {
    int written = 0;
    for (size_t at = 0; at + 6 <= s->size;) {
        const uint8_t *r = s->flags + at;
        uint32_t key;
        memcpy(&key, r, 4);
        int len = r[5];
        if (at + 6 + (size_t)len > s->size) break;
        written += flag_write(key, r[4], r + 6, len);
        at += 6 + (size_t)len;
    }
    if (!dbg_player_teleport_face(s->h.pos, s->h.face)) dbg_player_set_pos(s->h.pos);
    if (s->h.health >= 0) dbg_health_set(s->h.health);
    if (s->h.magic >= 0) dbg_magic_set(s->h.magic);
    snprintf(note, sizeof note, "State %d loaded: %d of %u flags changed", load_slot + 1, written, s->h.nflags);
}

int dbg_state_load(int slot) {
    if (slot < 0 || slot >= DBG_STATES) return 0;
    plat_mutex_lock(&lock);
    state_read_file(slot);
    State *s = &states[slot];
    int ok = 0;
    char cur[64];
    if (!s->set) snprintf(note, sizeof note, "State %d is empty", slot + 1);
    else if (!dbg_player_pos((float[3]){0})) snprintf(note, sizeof note, "Load a save first: there is no player");
    else if (dbg_current_area(cur) && !strcmp(cur, s->h.banc) && dbg_is_child() == s->h.child) {
        load_slot = slot, load_phase = L_IDLE;
        apply(s);
        ok = 1;
    } else {
        if (!dbg_warp_arm_to(s->h.banc, s->h.start) || !dbg_warp_now())
            snprintf(note, sizeof note, "The warp to %s did not start: %s", s->h.banc, dbg_warp_note());
        else {
            load_slot = slot, load_phase = L_ARRIVE, load_since = plat_now_ns();
            snprintf(note, sizeof note, "State %d: warping to %s", slot + 1, s->h.banc);
            ok = 1;
        }
    }
    plat_mutex_unlock(&lock);
    return ok;
}

void dbg_state_tick(uint64_t now_ns) {
    if (load_phase == L_IDLE) return;
    plat_mutex_lock(&lock);
    const State *s = &states[load_slot];
    char cur[64];
    float pos[3];
    int here = s->set && dbg_current_area(cur) && !strcmp(cur, s->h.banc) && dbg_player_pos(pos) &&
               dbg_flag_get_bool(dbg_flag_key("Sequence_IsLoadingBanc")) != 1;
    if (!s->set) load_phase = L_IDLE;
    else if (load_phase == L_ARRIVE) {
        if (here) load_phase = L_SETTLE, load_since = now_ns;
        else if (now_ns - load_since > ARRIVE_WAIT_NS) {
            load_phase = L_IDLE;
            snprintf(note, sizeof note, "State %d: the warp to %s did not arrive", load_slot + 1, s->h.banc);
        }
    } else if (load_phase == L_SETTLE) {
        if (!here) load_since = now_ns;
        else if (now_ns - load_since > SETTLE_NS) {
            load_phase = L_IDLE;
            apply(s);
            dbg_toast_show(note);
        }
    }
    plat_mutex_unlock(&lock);
}

int dbg_state_info(int slot, char banc[64], int64_t *saved_at) {
    if (slot < 0 || slot >= DBG_STATES) return 0;
    plat_mutex_lock(&lock);
    state_read_file(slot);
    const State *s = &states[slot];
    int set = s->set;
    if (set) {
        if (banc) memcpy(banc, s->h.banc, 64);
        if (saved_at) *saved_at = s->h.saved_at;
    }
    plat_mutex_unlock(&lock);
    return set;
}

int dbg_state_loading(void) { return load_phase != L_IDLE; }
const char *dbg_state_note(void) { return note; }
