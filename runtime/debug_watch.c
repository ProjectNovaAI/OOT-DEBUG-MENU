// Change monitor (debug_watch.h). Two helper threads: one reads every flag store whole every 50 ms (one memory read
// per store, debug_tools.c dbg_flag_store) and compares each entry's value with its last one; the other counts the
// loaded actors by name (debug_warp.c dbg_each_actor, about a second a pass) and logs the names whose count changed.
// A flag that changes more than NOISY times is ignored from then on, so timers do not fill the list.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "debug_watch.h"
#include "platform/platform.h"

#define POLL_NS 50000000ull
#define ACTOR_PAUSE_NS 500000000ull
#define NOISY 24
#define ACTOR_LINES 12   // more names changed in one pass than this: one line in the menu, the names in the file
#define NLOG 256
#define LINE 192
#define NAMES_FILE "debug_flag_names.json"
#define LOG_FILE "debug_changes.txt"

static const uint32_t kWidth[DBG_FLAG_TYPES] = {
    [DBG_FLAG_BOOL] = 1, [DBG_FLAG_INT] = 4, [DBG_FLAG_FLOAT] = 4, [DBG_FLAG_STRING] = 64, [DBG_FLAG_VEC3] = 12, [DBG_FLAG_ENUM] = 4,
};
static const char *const kType[DBG_FLAG_TYPES] = {"bool", "int", "float", "string", "vec3", "enum"};

static _Atomic int watch_on, flags_running, actors_running;
static _Atomic int want_reset, want_clear, want_ignore, actors_clear;
static _Atomic int ignored_n;
static _Atomic uint64_t t0;
static char state[96] = "off";

// ---- the log: a ring of lines, newest last, and the file
static PlatMutex log_lock;
static char log_ring[NLOG][LINE];
static int log_n, log_head;   // log_head: where the next line goes
static FILE *log_file;

// ring 0: the file only (the details of a summary line).
static void log_to(int ring, const char *fmt, va_list ap) {
    char line[LINE];
    double t = (double)(plat_now_ns() - atomic_load(&t0)) / 1e9;
    int k = snprintf(line, sizeof line, "%8.2f  ", t);
    vsnprintf(line + k, sizeof line - k, fmt, ap);
    plat_mutex_lock(&log_lock);
    if (ring) {
        memcpy(log_ring[log_head], line, LINE);
        log_head = (log_head + 1) % NLOG;
        if (log_n < NLOG) log_n++;
    }
    if (log_file) fprintf(log_file, "%s\n", line);
    plat_mutex_unlock(&log_lock);
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_to(1, fmt, ap);
    va_end(ap);
}

static void log_file_only(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_file_only(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_to(0, fmt, ap);
    va_end(ap);
}

static void log_flush(void) {
    plat_mutex_lock(&log_lock);
    if (log_file) fflush(log_file);
    plat_mutex_unlock(&log_lock);
}

int dbg_watch_count(void) {
    plat_mutex_lock_shared(&log_lock);
    int n = log_n;
    plat_mutex_unlock_shared(&log_lock);
    return n;
}

int dbg_watch_line(int i, char *out, size_t n) {
    int ok = 0;
    plat_mutex_lock_shared(&log_lock);
    if (i >= 0 && i < log_n) {
        snprintf(out, n, "%s", log_ring[(log_head - 1 - i + NLOG) % NLOG]);
        ok = 1;
    }
    plat_mutex_unlock_shared(&log_lock);
    return ok;
}

// ---- flag names: debug_flag_names.json (tools/debug_flag_names.py) maps "KEY": [kind, name, where]
static struct Name { uint32_t key; char *name; } *names;
static int nnames;
static _Atomic int names_loaded;

static int name_cmp(const void *a, const void *b) {
    uint32_t x = ((const struct Name *)a)->key, y = ((const struct Name *)b)->key;
    return x < y ? -1 : x > y;
}

// Reads a JSON string starting at the quote *p; leaves *p past it. Escapes are kept as the character after the backslash.
static int json_str(const char **p, char *out, size_t n) {
    const char *s = *p;
    if (*s != '"') return 0;
    size_t k = 0;
    for (s++; *s && *s != '"'; s++) {
        if (*s == '\\' && s[1]) s++;
        if (k + 1 < n) out[k++] = *s;
    }
    if (*s != '"') return 0;
    out[k] = 0;
    *p = s + 1;
    return 1;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    return s;
}

static void names_load(void) {
    if (atomic_exchange(&names_loaded, 1)) return;
    FILE *f = fopen(NAMES_FILE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = len > 0 ? malloc((size_t)len + 1) : NULL;
    if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) {
        free(text);
        fclose(f);
        return;
    }
    fclose(f);
    text[len] = 0;
    int cap = 0;
    for (const char *s = text; (s = strchr(s, '"'));) {
        char key[16], kind[32], name[160];
        if (!json_str(&s, key, sizeof key)) break;
        const char *q = skip_ws(s);
        if (strlen(key) != 8 || strspn(key, "0123456789abcdefABCDEF") != 8 || *q != ':') continue;
        q = skip_ws(q + 1);
        if (*q != '[') continue;
        q = skip_ws(q + 1);
        if (!json_str(&q, kind, sizeof kind)) continue;
        q = skip_ws(q);
        if (*q != ',') continue;
        q = skip_ws(q + 1);
        if (!json_str(&q, name, sizeof name)) continue;
        s = q;
        if (nnames == cap) {
            cap = cap ? cap * 2 : 4096;
            struct Name *g = realloc(names, (size_t)cap * sizeof *names);
            if (!g) break;
            names = g;
        }
        names[nnames].key = (uint32_t)strtoul(key, NULL, 16);
        names[nnames].name = strdup(name);
        if (names[nnames].name) nnames++;
    }
    free(text);
    qsort(names, (size_t)nnames, sizeof *names, name_cmp);
}

int dbg_watch_names(void) { return nnames; }

static const char *name_of(uint32_t key, char buf[16]) {
    struct Name want = {key, NULL}, *n = nnames ? bsearch(&want, names, (size_t)nnames, sizeof *names, name_cmp) : NULL;
    if (n) return n->name;
    snprintf(buf, 16, "key %08x", key);
    return buf;
}

// ---- flags
static struct Snap {
    uint64_t base;
    int count;
    uint32_t size, off;
    uint8_t *raw;      // the store as last read
    uint8_t *val;      // each entry's value, kWidth bytes apart
    uint32_t *key;
    uint8_t *muted;
    uint16_t *changes; // since the baseline or the last Clear
} snaps[DBG_FLAG_TYPES];

static void value_text(int t, const uint8_t *v, char *out, size_t n) {
    uint32_t u;
    float f[3];
    switch (t) {
    case DBG_FLAG_BOOL: snprintf(out, n, "%u", v[0]); break;
    case DBG_FLAG_INT: memcpy(&u, v, 4), snprintf(out, n, "%d", (int32_t)u); break;
    case DBG_FLAG_FLOAT: memcpy(f, v, 4), snprintf(out, n, "%.9g", f[0]); break;
    case DBG_FLAG_ENUM: memcpy(&u, v, 4), snprintf(out, n, "%08x", u); break;
    case DBG_FLAG_VEC3: memcpy(f, v, 12), snprintf(out, n, "(%.2f, %.2f, %.2f)", f[0], f[1], f[2]); break;
    default: {
        char s[65];
        memcpy(s, v, 64);
        s[64] = 0;
        snprintf(out, n, "\"%s\"", s);
    }
    }
}

// Reads store t whole and takes the values out of it. A store that moved or was not there is taken as the new
// baseline: returns 2 then, 1 when read as before, 0 when unreadable.
static int snap_read(int t) {
    struct Snap *s = &snaps[t];
    uint64_t base;
    int count;
    uint32_t size, off;
    if (!dbg_flag_store(t, &base, &count, &size, &off) || count <= 0) return 0;
    int fresh = base != s->base || count != s->count;
    if (fresh) {
        free(s->raw), free(s->val), free(s->key), free(s->muted), free(s->changes);
        memset(s, 0, sizeof *s);
        s->raw = malloc((size_t)count * size);
        s->val = malloc((size_t)count * kWidth[t]);
        s->key = malloc((size_t)count * sizeof *s->key);
        s->muted = calloc((size_t)count, 1);
        s->changes = calloc((size_t)count, sizeof *s->changes);
        if (!s->raw || !s->val || !s->key || !s->muted || !s->changes) return 0;
        s->base = base, s->count = count, s->size = size, s->off = off;
        dbg_flag_store_keys(t, s->key, count);
    }
    if (!plat_mem_read(base, s->raw, (size_t)count * size)) return 0;
    return fresh ? 2 : 1;
}

static void snap_take(int t) {
    struct Snap *s = &snaps[t];
    for (int i = 0; i < s->count; i++) memcpy(s->val + (size_t)i * kWidth[t], s->raw + (size_t)i * s->size + s->off, kWidth[t]);
}

static void snap_compare(int t) {
    struct Snap *s = &snaps[t];
    uint32_t w = kWidth[t];
    for (int i = 0; i < s->count; i++) {
        const uint8_t *now = s->raw + (size_t)i * s->size + s->off;
        uint8_t *was = s->val + (size_t)i * w;
        if (!memcmp(now, was, w)) continue;
        if (!s->muted[i]) {
            char a[80], b[80], nb[16];
            const char *nm = name_of(s->key[i], nb);
            value_text(t, was, a, sizeof a);
            value_text(t, now, b, sizeof b);
            if (s->changes[i] < 0xffff) s->changes[i]++;
            if (s->changes[i] > NOISY) {
                s->muted[i] = 1;
                atomic_fetch_add(&ignored_n, 1);
                log_line("%-6s %s changes all the time: ignored from now on", kType[t], nm);
            } else {
                log_line("%-6s %s  %s -> %s", kType[t], nm, a, b);
            }
        }
        memcpy(was, now, w);
    }
}

static void flags_thread(void *arg) {
    (void)arg;
    int found[DBG_FLAG_TYPES] = {0};
    while (atomic_load(&watch_on)) {
        if (atomic_exchange(&want_reset, 0)) {
            for (int t = 0; t < DBG_FLAG_TYPES; t++) snaps[t].base = 0, snaps[t].count = 0, found[t] = 0;
            atomic_store(&ignored_n, 0);
        }
        int clear = atomic_exchange(&want_clear, 0), ignore = atomic_exchange(&want_ignore, 0), any = 0;
        for (int t = 0; t < DBG_FLAG_TYPES; t++) {
            struct Snap *s = &snaps[t];
            if (ignore)
                for (int i = 0; i < s->count; i++)
                    if (s->changes[i] && !s->muted[i]) s->muted[i] = 1, atomic_fetch_add(&ignored_n, 1);
            if (clear || ignore)
                for (int i = 0; i < s->count; i++) s->changes[i] = 0;
            int r = snap_read(t);
            if (!r) continue;
            any = 1;
            if (r == 2 || clear) {
                if (r == 2 && found[t]++) log_line("%-6s the store moved (the game loaded again): new baseline", kType[t]);
                snap_take(t);
            } else {
                snap_compare(t);
            }
        }
        snprintf(state, sizeof state, any ? "Watching" : dbg_flags_locating() ? "Looking for the game's flags" : "Flags not found yet (load a save)");
        log_flush();
        plat_sleep_ns(POLL_NS);
    }
    atomic_store(&flags_running, 0);
}

// ---- actors, counted by name
struct ActorCount {
    char name[80];
    int n;
};
#define NACTOR_NAMES 2048
static struct ActorCount *act_now, *act_was;
static int act_now_n, act_was_n;

static int actor_count(uint64_t actor, const char *name, void *ctx) {
    (void)actor, (void)ctx;
    for (int i = 0; i < act_now_n; i++)
        if (!strcmp(act_now[i].name, name)) return act_now[i].n++, 1;
    if (act_now_n < NACTOR_NAMES) {
        snprintf(act_now[act_now_n].name, sizeof act_now[0].name, "%s", name);
        act_now[act_now_n++].n = 1;
    }
    return atomic_load(&watch_on);
}

static int count_in(const struct ActorCount *a, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(a[i].name, name)) return a[i].n;
    return 0;
}

static void actors_thread(void *arg) {
    (void)arg;
    if (!act_now) act_now = malloc(NACTOR_NAMES * sizeof *act_now);
    if (!act_was) act_was = malloc(NACTOR_NAMES * sizeof *act_was);
    int have = 0;
    while (act_now && act_was && atomic_load(&watch_on)) {
        if (atomic_exchange(&actors_clear, 0)) have = 0;
        act_now_n = 0;
        dbg_each_actor(actor_count, NULL);
        if (!atomic_load(&watch_on)) break;
        if (have && act_now_n) {
            // A pass with many names changed (an area loading) gets one line in the menu, the names in the file.
            int changed = 0;
            for (int i = 0; i < act_now_n; i++) changed += count_in(act_was, act_was_n, act_now[i].name) != act_now[i].n;
            for (int i = 0; i < act_was_n; i++) changed += !count_in(act_now, act_now_n, act_was[i].name);
            void (*put)(const char *, ...) = changed > ACTOR_LINES ? log_file_only : log_line;
            if (changed > ACTOR_LINES) log_line("actor  %d kinds of actor changed (an area loading?): the list is in " LOG_FILE, changed);
            for (int i = 0; i < act_now_n; i++) {
                int was = count_in(act_was, act_was_n, act_now[i].name);
                if (was != act_now[i].n) put("actor  %+d %s (now %d)", act_now[i].n - was, act_now[i].name, act_now[i].n);
            }
            for (int i = 0; i < act_was_n; i++)
                if (!count_in(act_now, act_now_n, act_was[i].name)) put("actor  -%d %s (now 0)", act_was[i].n, act_was[i].name);
        }
        if (act_now_n) {
            memcpy(act_was, act_now, (size_t)act_now_n * sizeof *act_now);
            act_was_n = act_now_n;
            have = 1;
        }
        plat_sleep_ns(ACTOR_PAUSE_NS);
    }
    atomic_store(&actors_running, 0);
}

// ---- control
void dbg_watch_set(int on) {
    if (!on) {
        atomic_store(&watch_on, 0);
        snprintf(state, sizeof state, "off");
        plat_mutex_lock(&log_lock);
        if (log_file) fclose(log_file), log_file = NULL;
        plat_mutex_unlock(&log_lock);
        return;
    }
    if (!dbg_available() || atomic_exchange(&watch_on, 1)) return;
    names_load();
    atomic_store(&t0, plat_now_ns());
    plat_mutex_lock(&log_lock);
    log_n = log_head = 0;
    if (!log_file) log_file = fopen(LOG_FILE, "w");
    plat_mutex_unlock(&log_lock);
    atomic_store(&want_reset, 1);
    atomic_store(&actors_clear, 1);
    snprintf(state, sizeof state, "Starting");
    log_line("watching: %d flag names known%s", nnames, nnames ? "" : " (no " NAMES_FILE " next to the game: keys only)");
    int zero = 0;
    if (atomic_compare_exchange_strong(&flags_running, &zero, 1) && !plat_thread_spawn(flags_thread, NULL, "debug-watch", 0))
        atomic_store(&flags_running, 0);
    zero = 0;
    if (atomic_compare_exchange_strong(&actors_running, &zero, 1) && !plat_thread_spawn(actors_thread, NULL, "debug-watch-actors", 0))
        atomic_store(&actors_running, 0);
}

int dbg_watch_on(void) { return atomic_load(&watch_on); }

void dbg_watch_ignore_changed(void) { atomic_store(&want_ignore, 1); }

void dbg_watch_clear(void) {
    plat_mutex_lock(&log_lock);
    log_n = log_head = 0;
    plat_mutex_unlock(&log_lock);
    atomic_store(&want_clear, 1);
    atomic_store(&actors_clear, 1);
}

int dbg_watch_ignored(void) { return atomic_load(&ignored_n); }

const char *dbg_watch_state(void) { return state; }
