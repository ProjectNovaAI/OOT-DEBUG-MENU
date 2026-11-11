// File-based remote console to the running game (debug_remote.h). A helper thread polls debug_cmd.txt; when it exists the
// commands in it run one per line and the answers go to debug_out.txt, which ends with "END". No socket is opened, so
// nothing is reachable from the network: only whoever can write to the game's folder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_remote.h"
#include "debug_scan.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <direct.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#define GETCWD getcwd
#endif

#define CMD_FILE "debug_cmd.txt"
#define OUT_FILE "debug_out.txt"
#define READY_FILE "debug_ready.txt"
#define MAX_READ 65536
#define MAX_FIND 64

static volatile int enabled, started;

static char *out_buf;
static size_t out_len, out_cap;

static void out_put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void out_put(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    if (out_len + n + 1 > out_cap) {
        size_t cap = out_cap ? out_cap * 2 : 65536;
        while (cap < out_len + n + 1) cap *= 2;
        char *p = realloc(out_buf, cap);
        if (!p) return;
        out_buf = p;
        out_cap = cap;
    }
    memcpy(out_buf + out_len, tmp, n);
    out_len += n;
    out_buf[out_len] = 0;
}

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        int a = hexval(s[0]), b = hexval(s[1]);
        if (a < 0 || b < 0) break;
        out[n++] = (uint8_t)(a << 4 | b);
        s += 2;
    }
    return n;
}

// Calls fn(start, end, prot) for every committed run of pages.
static void each_region(void (*fn)(uint64_t, uint64_t, int, void *), void *ctx) {
    uint64_t addr = 0x10000;
    while (addr < (1ull << 47)) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, 1ull << 47, &end);
        if (end <= addr) break;
        if (st == PLAT_PROT_READ || st == PLAT_PROT_RW) fn(addr, end, st, ctx);
        addr = end;
    }
}

static void region_print(uint64_t a, uint64_t e, int prot, void *ctx) {
    (void)ctx;
    out_put("0x%llx-0x%llx %s %lluK\n", (unsigned long long)a, (unsigned long long)e, prot == PLAT_PROT_RW ? "rw" : "r",
            (unsigned long long)((e - a) >> 10));
}

struct find_ctx {
    uint8_t pat[64];
    size_t len, align;
    int found;
    int want_rw_only;
};

static void region_find(uint64_t a, uint64_t e, int prot, void *vctx) {
    struct find_ctx *c = vctx;
    if (c->found >= MAX_FIND || (c->want_rw_only && prot != PLAT_PROT_RW) || e - a < c->len) return;
    enum { CHUNK = 1 << 20 };
    static uint8_t buf[CHUNK + 64];
    for (uint64_t at = a; at < e && c->found < MAX_FIND;) {
        uint64_t n = e - at < CHUNK + c->len - 1 ? e - at : CHUNK + c->len - 1;   // overlap so a hit on the seam is not missed
        if (n < c->len) break;
        if (!plat_mem_read(at, buf, n)) { at += CHUNK; continue; }   // reads without faulting; GPU-mapped pages can fault
        for (uint64_t i = 0; i + c->len <= n && i < CHUNK && c->found < MAX_FIND; i += c->align)
            if (buf[i] == c->pat[0] && !memcmp(buf + i, c->pat, c->len)) {
                out_put("0x%llx (main%+lld)\n", (unsigned long long)(at + i), (long long)(at + i - MOD_MAIN_BASE));
                c->found++;
            }
        at += CHUNK;
    }
}


// ---- write/read watchpoints. A watched page is made read-only (or no-access to catch reads too); the first access
// faults, is logged with the faulting host address and the return addresses found on the stack, and the page is
// opened so the access runs again. The helper thread closes it again a millisecond later, so several accesses inside
// that millisecond are not all logged ("faults" counts the real ones). Commands: watch, unwatch, wlog.
#define MAX_WATCH 4
#define LOG_CAP 256
#define STACK_KEEP 14
struct watch_entry {
    uint64_t addr, len;
    int mode;               // 1: writes, 2: reads and writes
    uint64_t open_page;     // the page a fault opened, to close again (0: none)
};
struct log_entry {
    uint64_t seq, addr, ip, before, tid, now_ns;
    int write, nstack;
    uint64_t stack[STACK_KEEP];
};
static struct watch_entry watches[MAX_WATCH];
static int nwatch;
static struct log_entry wlog[LOG_CAP];
static uint64_t wseq, wfaults;
static volatile int wlock;
static uint64_t anchor_lo, anchor_hi;   // return addresses are taken from this range around this program's own code

int mem_access_fault(uint64_t a, int write, uint64_t ip);   // memory.c

static int watch_hook(uint64_t addr, int write, uint64_t ip) {
    uint64_t page = addr & ~0xFFFull;
    int hit = -1;
    for (int i = 0; i < nwatch; i++)
        if (page == (watches[i].addr & ~0xFFFull) || page == ((watches[i].addr + watches[i].len - 1) & ~0xFFFull)) hit = i;
    if (hit < 0) return mem_access_fault(addr, write, ip);
    __atomic_fetch_add(&wfaults, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < nwatch; i++)
        if (addr >= watches[i].addr && addr < watches[i].addr + watches[i].len && (write || watches[i].mode == 2)) {
            while (__atomic_exchange_n(&wlock, 1, __ATOMIC_ACQUIRE)) {}
            uint64_t n = wseq++;
            struct log_entry *e = &wlog[n % LOG_CAP];
            uint64_t left = watches[i].addr + watches[i].len - addr;
            e->seq = n;
            e->addr = addr;
            e->ip = ip;
            e->write = write;
            e->before = 0;
            memcpy(&e->before, (void *)(uintptr_t)addr, left < 8 ? (size_t)left : 8);
            e->tid = plat_thread_id();
            e->now_ns = plat_now_ns();
            e->nstack = 0;
            volatile uint64_t marker = 0;
            const uint64_t *sp = (const uint64_t *)&marker;
            for (int k = 0; k < 4096 && e->nstack < STACK_KEEP; k++)   // the exception frames, then the faulting thread's stack
                if (sp[k] >= anchor_lo && sp[k] < anchor_hi) e->stack[e->nstack++] = sp[k];
            __atomic_store_n(&wlock, 0, __ATOMIC_RELEASE);
            break;
        }
    plat_mem_protect(page, 0x1000, PLAT_PROT_RW);
    watches[hit].open_page = page;
    return 1;
}

static void watch_rearm(void) {
    for (int i = 0; i < nwatch; i++) {
        uint64_t pg = __atomic_exchange_n(&watches[i].open_page, 0, __ATOMIC_ACQ_REL);
        if (pg) plat_mem_protect(pg, 0x1000, watches[i].mode == 2 ? PLAT_PROT_NONE : PLAT_PROT_READ);
    }
}

static void watch_clear(void) {
    int n = nwatch;
    nwatch = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = watches[i].addr & ~0xFFFull, e = (watches[i].addr + watches[i].len + 0xFFF) & ~0xFFFull;
        plat_mem_protect(a, e - a, PLAT_PROT_RW);
        watches[i].open_page = 0;
    }
}


// Hardware data breakpoints (platform.h plat_hw_watch): precise, one log entry per access, with the real stack.
static int hw_used;
static void hw_trap(int slot, uint64_t addr, uint64_t ip, uint64_t sp) {
    (void)slot;
    while (__atomic_exchange_n(&wlock, 1, __ATOMIC_ACQUIRE)) {}
    uint64_t n = wseq++;
    struct log_entry *e = &wlog[n % LOG_CAP];
    e->seq = n;
    e->addr = addr;
    e->ip = ip;
    e->write = 1;
    e->before = 0;
    memcpy(&e->before, (void *)(uintptr_t)addr, 8);   // the value after the access
    e->tid = plat_thread_id();
    e->now_ns = plat_now_ns();
    e->nstack = 0;
    const uint64_t *p = (const uint64_t *)(uintptr_t)sp;
    for (int k = 0; k < 2048 && e->nstack < STACK_KEEP; k++)
        if (p[k] >= anchor_lo && p[k] < anchor_hi) e->stack[e->nstack++] = p[k];
    __atomic_fetch_add(&wfaults, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&wlock, 0, __ATOMIC_RELEASE);
}

static uint64_t num(const char *s) { return strtoull(s, NULL, 0); }

static void run_command(char *line) {
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd) return;
    char *a1 = strtok(NULL, " \t\r\n"), *a2 = strtok(NULL, " \t\r\n");
    char *a3 = strtok(NULL, " \t\r\n");
    out_put("> %s%s%s%s%s\n", cmd, a1 ? " " : "", a1 ? a1 : "", a2 ? " " : "", a2 ? a2 : "");
    if (!strcmp(cmd, "main")) {
        out_put("main base 0x%llx, v111 %d\n", (unsigned long long)MOD_MAIN_BASE, dbg_available());
    } else if (!strcmp(cmd, "anchor")) {   // the host address of this program's code: map symbol dbg_remote_set minus this = slide
        out_put("anchor dbg_remote_set %p\n", (void *)dbg_remote_set);
    } else if (!strcmp(cmd, "watch") && a1) {   // watch ADDR [LEN] [w|rw]: log the next accesses to these bytes
        if (nwatch >= MAX_WATCH) { out_put("too many watches (%d); unwatch first\n", MAX_WATCH); return; }
        uint64_t a = num(a1), n = a2 ? num(a2) : 8;
        int mode = a3 && !strcmp(a3, "rw") ? 2 : 1;
        uint64_t end;
        if (!n || n > 0x10000 || plat_mem_state(a & ~0xFFFull, ((a + n) & ~0xFFFull) + 0x1000, &end) != PLAT_PROT_RW) {
            out_put("not writable memory\n");
            return;
        }
        anchor_lo = (uint64_t)(uintptr_t)dbg_remote_set - (400ull << 20);
        anchor_hi = (uint64_t)(uintptr_t)dbg_remote_set + (400ull << 20);
        plat_mem_access_hook(watch_hook);
        watches[nwatch].addr = a;
        watches[nwatch].len = n;
        watches[nwatch].mode = mode;
        watches[nwatch].open_page = 0;
        nwatch++;
        plat_mem_protect(a & ~0xFFFull, ((a + n - 1) & ~0xFFFull) + 0x1000 - (a & ~0xFFFull), mode == 2 ? PLAT_PROT_NONE : PLAT_PROT_READ);
        out_put("watching 0x%llx +%llu (%s)\n", (unsigned long long)a, (unsigned long long)n, mode == 2 ? "reads and writes" : "writes");
    } else if (!strcmp(cmd, "hwatch") && a1) {   // hwatch ADDR [LEN] [w|rw]: precise hardware breakpoints, up to 4 aligned 8-byte pieces
        uint64_t a = num(a1), n = a2 ? num(a2) : 8;
        int rw = a3 && !strcmp(a3, "rw") ? 3 : 1;
        anchor_lo = (uint64_t)(uintptr_t)dbg_remote_set - (400ull << 20);
        anchor_hi = (uint64_t)(uintptr_t)dbg_remote_set + (400ull << 20);
        plat_hw_watch_hook(hw_trap);
        uint64_t at = a & ~7ull, end = (a + n + 7) & ~7ull;
        int used = 0, threads = 0;
        for (; at < end && used < 4; at += 8, used++) threads = plat_hw_watch(used, at, 8, rw);
        hw_used = used;
        out_put("hardware watch on %d piece(s) from 0x%llx, %d threads armed%s\n", used, (unsigned long long)(a & ~7ull), threads,
                at < end ? " (range longer than 32 bytes: the end is not watched)" : "");
    } else if (!strcmp(cmd, "hunwatch")) {
        for (int i = 0; i < 4; i++) plat_hw_watch(i, 0, 0, 1);
        hw_used = 0;
        out_put("hardware watches cleared\n");
    } else if (!strcmp(cmd, "unwatch")) {
        watch_clear();
        out_put("watches cleared\n");
    } else if (!strcmp(cmd, "wlog")) {   // wlog [clear]: the logged accesses, oldest first
        if (a1 && !strcmp(a1, "clear")) { wseq = 0; out_put("log cleared\n"); return; }
        uint64_t first = wseq > LOG_CAP ? wseq - LOG_CAP : 0;
        out_put("%llu accesses logged, %llu faults\n", (unsigned long long)wseq, (unsigned long long)wfaults);
        for (uint64_t n = first; n < wseq; n++) {
            struct log_entry *e = &wlog[n % LOG_CAP];
            out_put("#%llu %s addr 0x%llx ip %p tid %llu t %llu before %016llx stack", (unsigned long long)e->seq, e->write ? "W" : "R",
                    (unsigned long long)e->addr, (void *)(uintptr_t)e->ip, (unsigned long long)e->tid,
                    (unsigned long long)(e->now_ns / 1000000), (unsigned long long)e->before);
            for (int k = 0; k < e->nstack; k++) out_put(" %p", (void *)(uintptr_t)e->stack[k]);
            out_put("\n");
        }
    } else if (!strcmp(cmd, "dump") && a1 && a2 && a3) {   // dump ADDR LEN FILE: raw bytes to a file next to the game
        uint64_t a = num(a1), n = num(a2);
        FILE *f = n <= (1ull << 30) ? fopen(a3, "wb") : NULL;
        if (!f) { out_put("cannot write\n"); return; }
        static uint8_t buf[1 << 20];
        for (uint64_t done = 0; done < n; done += sizeof buf) {
            size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf;
            if (!plat_mem_read(a + done, buf, k)) memset(buf, 0, k);
            fwrite(buf, 1, k, f);
        }
        fclose(f);
        out_put("wrote %llu bytes to %s\n", (unsigned long long)n, a3);
    } else if (!strcmp(cmd, "vtable") && a1) {   // vtable OBJECT [N]: the object's first qword as a table of N guest function addresses
        uint64_t obj = num(a1), n = a2 ? num(a2) : 24, vt;
        if (!plat_mem_read(obj, &vt, 8)) { out_put("unreadable\n"); return; }
        out_put("vtable 0x%llx (main%+lld)\n", (unsigned long long)vt, (long long)(vt - MOD_MAIN_BASE));
        for (uint64_t i = 0; i < n; i++) {
            uint64_t f;
            if (!plat_mem_read(vt + i * 8, &f, 8)) break;
            out_put("  [%llu] 0x%llx (main%+lld)\n", (unsigned long long)i, (unsigned long long)f, (long long)(f - MOD_MAIN_BASE));
        }
    } else if (!strcmp(cmd, "regions")) {
        each_region(region_print, NULL);
    } else if (!strcmp(cmd, "read") && a1) {   // read ADDR [LEN]
        uint64_t a = num(a1), n = a2 ? num(a2) : 64;
        if (n > MAX_READ) n = MAX_READ;
        if (!readable(a, n)) { out_put("not readable\n"); return; }
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        for (uint64_t i = 0; i < n; i += 16) {
            out_put("%llx:", (unsigned long long)(a + i));
            for (uint64_t j = i; j < i + 16 && j < n; j++) out_put(" %02x", p[j]);
            out_put("\n");
        }
    } else if (!strcmp(cmd, "ptrs") && a1) {   // ptrs ADDR [COUNT]: 8-byte values, main-relative when inside the module
        uint64_t a = num(a1), n = a2 ? num(a2) : 16;
        if (n > 4096) n = 4096;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t v;
            if (!plat_mem_read(a + i * 8, &v, 8)) { out_put("%llx: unreadable\n", (unsigned long long)(a + i * 8)); break; }
            out_put("%llx: %016llx%s\n", (unsigned long long)(a + i * 8), (unsigned long long)v,
                    v >= MOD_MAIN_BASE && v < MOD_MAIN_BASE + 0x6000000 ? " (main+)" : "");
        }
    } else if (!strcmp(cmd, "chain") && a1) {   // chain ADDR off off ...: follow pointers (read at ADDR, add off, read ...)
        uint64_t cur = num(a1);
        char *offs[16];
        int no = 0;
        if (a2) offs[no++] = a2;
        if (a3) offs[no++] = a3;
        char *t;
        while (no < 16 && (t = strtok(NULL, " \t\r\n"))) offs[no++] = t;
        out_put("start 0x%llx\n", (unsigned long long)cur);
        for (int i = 0; i < no; i++) {
            uint64_t v;
            if (!plat_mem_read(cur, &v, 8)) { out_put("unreadable at 0x%llx\n", (unsigned long long)cur); return; }
            cur = v + num(offs[i]);
            out_put("-> 0x%llx\n", (unsigned long long)cur);
        }
    } else if (!strcmp(cmd, "str") && a1) {   // str ADDR: a C string
        char s[257];
        uint64_t a = num(a1);
        size_t n = 0;
        while (n < 256 && plat_mem_read(a + n, &s[n], 1) && s[n]) n++;
        s[n] = 0;
        out_put("%s\n", s);
    } else if (!strcmp(cmd, "write") && a1 && a2) {   // write ADDR HEXBYTES
        uint8_t b[256];
        uint64_t a = num(a1);
        size_t n = parse_hex(a2, b, sizeof b), end;
        if (!n || plat_mem_state(a & ~0xFFFull, ((a + n) & ~0xFFFull) + 0x1000, &end) != PLAT_PROT_RW) { out_put("not writable\n"); return; }
        plat_mem_prepare_write(a, n);
        memcpy((void *)(uintptr_t)a, b, n);
        out_put("wrote %zu bytes\n", n);
    } else if (!strcmp(cmd, "find") && a1) {   // find HEXBYTES [rw]: the pattern anywhere in memory (aligned to its size up to 8)
        struct find_ctx c;
        memset(&c, 0, sizeof c);
        c.len = parse_hex(a1, c.pat, sizeof c.pat);
        c.align = c.len >= 8 ? 8 : c.len >= 4 ? 4 : 1;
        c.want_rw_only = a2 && !strcmp(a2, "rw");
        if (!c.len) { out_put("bad pattern\n"); return; }
        each_region(region_find, &c);
        out_put("%d found%s\n", c.found, c.found >= MAX_FIND ? " (limit)" : "");
    } else if (!strcmp(cmd, "findptr") && a1) {   // findptr VALUE: 8-byte aligned qwords equal to VALUE
        struct find_ctx c;
        memset(&c, 0, sizeof c);
        uint64_t v = num(a1);
        memcpy(c.pat, &v, 8);
        c.len = 8;
        c.align = 8;
        each_region(region_find, &c);
        out_put("%d found%s\n", c.found, c.found >= MAX_FIND ? " (limit)" : "");
    } else if (!strcmp(cmd, "scan") && a1) {   // scan INT
        out_put("%d hits\n", dbg_scan_first((int32_t)strtol(a1, NULL, 0)));
    } else if (!strcmp(cmd, "scanf") && a1) {   // scanf FLOAT
        float f = strtof(a1, NULL);
        int32_t bits;
        memcpy(&bits, &f, 4);
        out_put("%d hits\n", dbg_scan_first(bits));
    } else if (!strcmp(cmd, "next") && a1) {   // next eq|changed|same|up|down [INT]
        int mode = !strcmp(a1, "changed") ? SCAN_CHANGED : !strcmp(a1, "same") ? SCAN_UNCHANGED : !strcmp(a1, "up") ? SCAN_INCREASED : !strcmp(a1, "down") ? SCAN_DECREASED : SCAN_EQUAL;
        out_put("%d hits\n", dbg_scan_next(mode, a2 ? (int32_t)strtol(a2, NULL, 0) : 0));
    } else if (!strcmp(cmd, "hits")) {   // hits [N]
        int n = dbg_scan_count(), lim = a1 ? (int)num(a1) : 32;
        out_put("%d hits\n", n);
        for (int i = 0; i < n && i < lim; i++) {
            uint64_t a;
            int32_t v;
            if (dbg_scan_get(i, &a, &v)) out_put("0x%llx (main%+lld) = %d\n", (unsigned long long)a, (long long)(a - MOD_MAIN_BASE), v);
        }
    } else if (!strcmp(cmd, "set") && a1 && a2) {   // set HITINDEX INT
        out_put("%s\n", dbg_scan_write((int)num(a1), (int32_t)strtol(a2, NULL, 0)) ? "written" : "failed");
    } else if (!strcmp(cmd, "player")) {
        float p[3];
        if (dbg_player_pos(p)) out_put("player %.3f %.3f %.3f speed %.2f vert %.2f\n", p[0], p[1], p[2], dbg_ground_speed(), dbg_vertical_speed());
        else out_put("no player\n");
    } else if (!strcmp(cmd, "teleport") && a1 && a2 && a3) {
        float p[3] = {strtof(a1, NULL), strtof(a2, NULL), strtof(a3, NULL)};
        out_put("%s\n", dbg_player_set_pos(p) ? "moved" : "failed");
    } else if (!strcmp(cmd, "speed") && a1) {
        dbg_set_time_scale(strtof(a1, NULL));
        out_put("speed %.3f\n", dbg_time_scale());
    } else {
        out_put("unknown or incomplete command. Commands: hwatch hunwatch anchor watch unwatch wlog dump vtable main regions read ptrs chain str write find findptr scan scanf next hits set player teleport speed\n");
    }
}

static void remote_thread(void *arg) {
    (void)arg;
    char dir[1024] = "";
    if (GETCWD(dir, sizeof dir)) {
        FILE *f = fopen(READY_FILE, "w");
        if (f) {
            fprintf(f, "%s\n", dir);
            fclose(f);
        }
    }
    for (;;) {
        plat_sleep_ns(nwatch ? 1000ull * 1000 : 50ull * 1000 * 1000);
        if (nwatch) watch_rearm();
        if (!enabled) continue;
        FILE *f = fopen(CMD_FILE, "rb");
        if (!f) continue;
        char *text = malloc(1 << 20);
        size_t n = text ? fread(text, 1, (1 << 20) - 1, f) : 0;
        fclose(f);
        remove(CMD_FILE);
        if (!text) continue;
        text[n] = 0;
        out_len = 0;
        char *save = text;
        for (char *line = text; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            run_command(line);
            line = nl ? nl + 1 : NULL;
        }
        out_put("END\n");
        free(save);
        FILE *o = fopen(OUT_FILE ".tmp", "wb");
        if (o) {
            fwrite(out_buf, 1, out_len, o);
            fclose(o);
            remove(OUT_FILE);
            rename(OUT_FILE ".tmp", OUT_FILE);
        }
    }
}

void dbg_remote_set(int on) {
    enabled = on != 0;
    if (on && !started) {
        started = 1;
        plat_thread_spawn(remote_thread, NULL, "debug-remote", 0);
    }
}

int dbg_remote_enabled(void) { return enabled; }
