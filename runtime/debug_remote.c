// File-based remote console to the running game (debug_remote.h). A helper thread polls debug_cmd.txt; when it exists the
// commands in it run one per line and the answers go to debug_out.txt, which ends with "END". No socket is opened, so
// nothing is reachable from the network: only whoever can write to the game's folder.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "clip.h"
#include "debug_remote.h"
#include "debug_scan.h"
#include "debug_tools.h"
#include "debug_watch.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"
#include "save_slots.h"
#include "vk_debug_cam.h"

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

static void fcount_line(uint64_t addr, uint32_t n, uint64_t lr, double first_s, void *ctx) {
    (void)ctx;
    char d[128];
    out_put("0x%llx %u from %s first %.4f\n", (unsigned long long)addr, n, ldr_describe(lr, d, sizeof d), first_s);
}

// The key of a flag named on the console: "#" and eight hex digits is the key itself, for the flags whose names are not
// known (a struct member found only by its hash in the flag list); anything else is a name, hashed as the game does.
static uint32_t remote_flag_key(const char *name) {
    if (name[0] == '#' && strlen(name) == 9 && strspn(name + 1, "0123456789abcdefABCDEF") == 8)
        return (uint32_t)strtoul(name + 1, NULL, 16);
    return dbg_flag_key(name);
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
    uint64_t from;   // hits below this address are skipped, so a search past MAX_FIND can go on from the last hit
};

static void region_find(uint64_t a, uint64_t e, int prot, void *vctx) {
    struct find_ctx *c = vctx;
    if (e <= c->from) return;
    if (a < c->from) a = c->from & ~(uint64_t)7;
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
#define STACK_KEEP 40
struct watch_entry {
    uint64_t addr, len;
    int mode;               // 1: writes, 2: reads and writes
    uint64_t open_page;     // the page a fault opened, to close again (0: none)
};
struct log_entry {
    uint64_t seq, addr, ip, before, tid, now_ns;
    int write, nstack;
    uint64_t stack[STACK_KEEP];
    uint64_t regs[9];       // execution breakpoints in lifted code: guest x0..x8
    uint64_t guest_sp;
    char text[3][40];       // C strings x1..x3 pointed to, when printable
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
static int hw_exec[4];   // slot is an execution breakpoint
static void hw_trap(int slot, uint64_t addr, uint64_t ip, uint64_t sp, uint64_t arg0) {
    while (__atomic_exchange_n(&wlock, 1, __ATOMIC_ACQUIRE)) {}
    uint64_t n = wseq++;
    struct log_entry *e = &wlog[n % LOG_CAP];
    e->seq = n;
    e->addr = addr;
    e->ip = ip;
    e->write = hw_exec[slot & 3] ? 2 : 1;
    e->before = 0;
    memcpy(&e->before, (void *)(uintptr_t)addr, 8);   // the value after the access; for an execution breakpoint the code bytes
    e->tid = plat_thread_id();
    e->now_ns = plat_now_ns();
    e->nstack = 0;
    memset(e->regs, 0, sizeof e->regs);
    memset(e->text, 0, sizeof e->text);
    e->guest_sp = 0;
    if (hw_exec[slot & 3] && arg0) {   // the Cpu * of the lifted function: its guest registers are the call's arguments
        plat_mem_read(arg0, e->regs, sizeof e->regs);
        plat_mem_read(arg0 + 256, &e->guest_sp, 8);
        for (int k = 0; k < 3; k++) {
            char tmp[40];
            uint64_t a = e->regs[k + 1];
            if (a > 0x10000 && plat_mem_read(a, tmp, sizeof tmp - 1)) {
                int n = 0;
                while (n < 39 && tmp[n] >= 32 && tmp[n] < 127) n++;
                if (n >= 3) { memcpy(e->text[k], tmp, n); e->text[k][n] = 0; }
            }
        }
    }
    for (uint64_t at = sp; at < sp + 65536 && e->nstack < STACK_KEEP; at += 512) {   // read safely: the stack may end before 64 KiB
        uint64_t q[64];
        if (!plat_mem_read(at, q, sizeof q)) break;
        for (int k = 0; k < 64 && e->nstack < STACK_KEEP; k++)
            if (q[k] >= anchor_lo && q[k] < anchor_hi) e->stack[e->nstack++] = q[k];
    }
    __atomic_fetch_add(&wfaults, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&wlock, 0, __ATOMIC_RELEASE);
}

static uint64_t num(const char *s) { return strtoull(s, NULL, 0); }

static void run_command(char *line) {
    static char whole[1024];   // the line before strtok cuts it: for an argument with spaces (saves)
    snprintf(whole, sizeof whole, "%s", line);
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd) return;
    char *a1 = strtok(NULL, " \t\r\n"), *a2 = strtok(NULL, " \t\r\n");
    char *a3 = strtok(NULL, " \t\r\n");
    out_put("> %s%s%s%s%s\n", cmd, a1 ? " " : "", a1 ? a1 : "", a2 ? " " : "", a2 ? a2 : "");
    if (!strcmp(cmd, "input")) {   // input <script lines, ';' between>: queue presses as if from the pad (input.h)
        const char *rest = strstr(whole, "input");
        rest = rest ? rest + 5 : "";
        if (a1) out_put("queued %d input events\n", input_script_live(rest));
        else out_put("%d input events pending\n", input_script_live_busy());
    } else if (!strcmp(cmd, "main")) {
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
        for (; at < end && used < 3; at += 8, used++) threads = plat_hw_watch(used, at, 8, rw);
        hw_used = used;
        out_put("hardware watch on %d piece(s) from 0x%llx, %d threads armed%s\n", used, (unsigned long long)(a & ~7ull), threads,
                at < end ? " (range longer than 32 bytes: the end is not watched)" : "");
    } else if (!strcmp(cmd, "hexec") && a1) {   // hexec HOSTADDR [SLOT 0-3]: log who calls the function at this host address
        uint64_t a = num(a1);
        int slot = a2 ? (int)num(a2) & 3 : 3;
        anchor_lo = (uint64_t)(uintptr_t)dbg_remote_set - (400ull << 20);
        anchor_hi = (uint64_t)(uintptr_t)dbg_remote_set + (400ull << 20);
        plat_hw_watch_hook(hw_trap);
        hw_exec[slot] = 1;
        out_put("execution breakpoint %d at 0x%llx, %d threads armed\n", slot, (unsigned long long)a, plat_hw_watch(slot, a, 1, 0));
    } else if (!strcmp(cmd, "hunwatch")) {
        for (int i = 0; i < 4; i++) { plat_hw_watch(i, 0, 0, 1); hw_exec[i] = 0; }
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
            out_put("#%llu %s addr 0x%llx ip %p tid %llu t %llu before %016llx stack", (unsigned long long)e->seq, e->write == 2 ? "X" : e->write ? "W" : "R",
                    (unsigned long long)e->addr, (void *)(uintptr_t)e->ip, (unsigned long long)e->tid,
                    (unsigned long long)(e->now_ns / 1000000), (unsigned long long)e->before);
            for (int k = 0; k < e->nstack; k++) out_put(" %p", (void *)(uintptr_t)e->stack[k]);
            out_put("\n");
            if (e->write == 2) {
                out_put("   regs");
                for (int k = 0; k < 9; k++) out_put(" x%d=%llx", k, (unsigned long long)e->regs[k]);
                out_put(" sp=%llx\n", (unsigned long long)e->guest_sp);
                for (int k = 0; k < 3; k++)
                    if (e->text[k][0]) out_put("   x%d points to \"%s\"\n", k + 1, e->text[k]);
            }
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
    } else if (!strcmp(cmd, "find") && a1) {
        // find HEXBYTES [rw] [FROM]: the pattern anywhere in memory (aligned to its size up to 8), at or above FROM
        struct find_ctx c;
        memset(&c, 0, sizeof c);
        c.len = parse_hex(a1, c.pat, sizeof c.pat);
        c.align = c.len >= 8 ? 8 : c.len >= 4 ? 4 : 1;
        c.want_rw_only = a2 && !strcmp(a2, "rw");
        const char *from = c.want_rw_only ? a3 : a2;
        if (from) c.from = num(from);
        if (!c.len) { out_put("bad pattern\n"); return; }
        each_region(region_find, &c);
        out_put("%d found%s\n", c.found, c.found >= MAX_FIND ? " (limit)" : "");
    } else if (!strcmp(cmd, "findptr") && a1) {   // findptr VALUE [FROM]: 8-byte aligned qwords equal to VALUE, at or above FROM
        struct find_ctx c;
        memset(&c, 0, sizeof c);
        uint64_t v = num(a1);
        memcpy(c.pat, &v, 8);
        c.len = 8;
        c.align = 8;
        if (a2) c.from = num(a2);
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
    } else if (!strcmp(cmd, "overlay") && a1) {   // overlay MASK: 1 position, 2 speed, 4 timer, 8 inputs, 16 area
        dbg_overlay_set((int)num(a1));
        out_put("overlay mask %d\n", dbg_overlay_mask());
    } else if (!strcmp(cmd, "hotkeys") && a1) {   // hotkeys 0|1: F5 save, F6 load, F7/F8 speed, F9 timer, F12 reload
        dbg_hotkeys_set((int)num(a1));
        out_put("hotkeys %d\n", dbg_hotkeys());
    } else if (!strcmp(cmd, "memory")) {   // memory: the application pool, what is in use now and the most so far, MiB
        uint64_t pool = 0, used = 0;
        mem_info(6, &pool);
        mem_info(7, &used);
        out_put("pool %llu MiB, in use %llu, peak %llu, free %lld\n", (unsigned long long)(pool >> 20),
                (unsigned long long)(used >> 20), (unsigned long long)(mem_used_peak() >> 20),
                (long long)(pool >> 20) - (long long)(used >> 20));
    } else if (!strcmp(cmd, "player")) {
        float p[3];
        if (dbg_player_pos(p)) out_put("player %.3f %.3f %.3f speed %.2f vert %.2f\n", p[0], p[1], p[2], dbg_ground_speed(), dbg_vertical_speed());
        else out_put("no player\n");
    } else if (!strcmp(cmd, "teleport") && a1 && a2 && a3) {
        float p[3] = {strtof(a1, NULL), strtof(a2, NULL), strtof(a3, NULL)};
        out_put("%s\n", dbg_player_set_pos(p) ? "moved" : "failed");
    } else if (!strcmp(cmd, "health")) {
        int c, m;
        if (dbg_health_get(&c, &m)) out_put("health %d of %d quarter hearts, invulnerable %d\n", c, m, dbg_invulnerable_on());
        else out_put("health: %s\n", dbg_health_locating() ? "searching" : "not found yet");
    } else if (!strcmp(cmd, "sethealth") && a1) {
        out_put("sethealth %s\n", dbg_health_set(atoi(a1)) ? "done" : "failed");
    } else if (!strcmp(cmd, "setmaxhealth") && a1) {
        out_put("setmaxhealth %s\n", dbg_health_set_max(atoi(a1)) ? "done" : "failed");
    } else if (!strcmp(cmd, "lock") && a1) {
        dbg_health_lock(atoi(a1));
        out_put("lock %d\n", dbg_health_lock_value());
    } else if (!strcmp(cmd, "nodes")) {
        out_put("replaced by: flag NAME [VALUE], items, item INDEX VALUE, magic [VALUE], infmagic 0|1, infitems 0|1\n");
    } else if (!strcmp(cmd, "flag") && a1) {   // flag NAME [VALUE]: an int flag by its name in the game's flag list
        int v = 0;
        uint32_t key = remote_flag_key(a1);
        if (a2) out_put("flag %s (key %08x) set %s\n", a1, key, dbg_flag_set(key, atoi(a2)) ? "done" : "failed");
        if (dbg_flag_get(key, &v)) out_put("flag %s (key %08x) = %d\n", a1, key, v);
        else out_put("flag %s (key %08x): %s\n", a1, key, dbg_flags_locating() ? "searching" : "not found");
    } else if (!strcmp(cmd, "items")) {
        for (int i = 0; i < dbg_item_count(); i++) {
            int v = 0, max = 0;
            const char *label = dbg_item_label(i, &max, NULL);
            if (dbg_item_get(i, &v)) out_put("%2d %s = %d (max %d)\n", i, label, v, max);
            else out_put("%2d %s: %s\n", i, label, dbg_flags_locating() ? "searching" : "not found");
        }
    } else if (!strcmp(cmd, "item") && a1 && a2) {
        out_put("item %s\n", dbg_item_set(atoi(a1), atoi(a2)) ? "done" : "failed");
    } else if (!strcmp(cmd, "magic")) {
        int c, m;
        if (a1) out_put("magic set %s\n", dbg_magic_set(atoi(a1)) ? "done" : "failed");
        if (dbg_magic_get(&c, &m)) out_put("magic %d of %d, infinite %d\n", c, m, dbg_magic_infinite_on());
        else out_put("magic: %s\n", dbg_flags_locating() ? "searching" : "not found");
    } else if (!strcmp(cmd, "infmagic") && a1) {
        dbg_magic_infinite(atoi(a1));
        out_put("infmagic %d\n", dbg_magic_infinite_on());
    } else if (!strcmp(cmd, "anyage")) {   // anyage [0|1]: Items at any age, and the item table entries noted
        if (a1) dbg_any_age_items(atoi(a1));
        int n = dbg_pouch_entries();
        out_put("anyage %d, %d table entries\n", dbg_any_age_items_on(), n);
        uint64_t ea;
        uint8_t own[6];
        for (int i = 0; i < n && dbg_pouch_entry_at(i, &ea, own); i++)
            out_put("  %#llx own %d%d keys %d%d%d%d\n", (unsigned long long)ea, own[0], own[1], own[2], own[3], own[4], own[5]);
    } else if (!strcmp(cmd, "infitems") && a1) {
        dbg_items_infinite(atoi(a1));
        out_put("infitems %d\n", dbg_items_infinite_on());
    } else if (!strcmp(cmd, "lank")) {
        if (a1) dbg_lank(atoi(a1));
        out_put("lank %d\n", dbg_lank_on());
    } else if (!strcmp(cmd, "god") && a1) {
        dbg_invulnerable(atoi(a1));
        out_put("god %d\n", dbg_invulnerable_on());
    } else if (!strcmp(cmd, "onehit")) {   // onehit [0|1]: one-hit kills
        if (a1) dbg_one_hit(atoi(a1));
        out_put("onehit %d\n", dbg_one_hit_on());
    } else if (!strcmp(cmd, "speed") && a1) {
        dbg_set_time_scale(strtof(a1, NULL));
        out_put("speed %.3f\n", dbg_time_scale());
    } else if (!strcmp(cmd, "fget") && a1) {   // fget NAME: any flag by name, from whichever store has it
        uint32_t key = remote_flag_key(a1), u = 0;
        int t = dbg_flag_type(key);
        char s[64];
        float v[3];
        if (t < 0) out_put("%s (key %08x): %s\n", a1, key, dbg_flags_locating() ? "searching" : "not found");
        else if (t == DBG_FLAG_BOOL) out_put("%s bool = %d\n", a1, dbg_flag_get_bool(key));
        else if (t == DBG_FLAG_STRING && dbg_flag_get_string(key, s)) out_put("%s string = \"%s\"\n", a1, s);
        else if (t == DBG_FLAG_VEC3 && dbg_flag_get_vec3(key, v)) out_put("%s vec3 = %g %g %g\n", a1, v[0], v[1], v[2]);
        else if (dbg_flag_get_u32(t, key, &u)) {
            float f;
            memcpy(&f, &u, 4);
            out_put("%s %s = %d (0x%08x, float %g)\n", a1, t == DBG_FLAG_INT ? "int" : t == DBG_FLAG_ENUM ? "enum" : "float", (int)u, u, f);
        }
    } else if (!strcmp(cmd, "fset") && a1 && a2) {   // fset NAME VALUE: bool 0/1, int, float, enum hash or value name, string (- for empty), vec3 x,y,z
        uint32_t key = remote_flag_key(a1);
        int t = dbg_flag_type(key), ok = 0;
        if (t == DBG_FLAG_BOOL) ok = dbg_flag_set_bool(key, atoi(a2));
        else if (t == DBG_FLAG_INT) ok = dbg_flag_set(key, atoi(a2));
        else if (t == DBG_FLAG_FLOAT) {
            float f = strtof(a2, NULL);
            uint32_t u;
            memcpy(&u, &f, 4);
            ok = dbg_flag_set_u32(t, key, u);
        } else if (t == DBG_FLAG_ENUM) {
            ok = dbg_flag_set_u32(t, key, isdigit((unsigned char)a2[0]) ? (uint32_t)strtoul(a2, NULL, 0) : dbg_flag_key(a2));
        } else if (t == DBG_FLAG_STRING) {
            ok = dbg_flag_set_string(key, strcmp(a2, "-") ? a2 : "");
        } else if (t == DBG_FLAG_VEC3) {
            float v[3] = {0};
            sscanf(a2, "%f,%f,%f", &v[0], &v[1], &v[2]);
            ok = dbg_flag_set_vec3(key, v);
        }
        out_put("fset %s: %s\n", a1, ok ? "done" : t < 0 ? "flag not found" : "failed");
    } else if (!strcmp(cmd, "warps")) {   // warps [FILTER]: the destinations
        for (int i = 0; i < dbg_warp_count(); i++)
            if (!a1 || strstr(dbg_warp_banc(i), a1) || strstr(dbg_warp_start(i), a1)) out_put("%3d %s %s\n", i, dbg_warp_banc(i), dbg_warp_start(i));
    } else if (!strcmp(cmd, "warp") && a1) {   // warp INDEX [now]: arm a warp (and with now, start it)
        int i = atoi(a1);
        out_put("warp %d: %s\n", i, dbg_warp_arm(i) ? "armed" : "failed");
        if (a2 && !strcmp(a2, "now")) out_put("instant: %s\n", dbg_warp_now() ? "requested" : "failed");
        out_put("%s\n", dbg_warp_note());
    } else if (!strcmp(cmd, "warpcancel")) {
        dbg_warp_cancel();
        out_put("%s\n", dbg_warp_note());
    } else if (!strcmp(cmd, "own")) {   // own [FILTER]: the owned-items list with each one's state
        for (int i = 0; i < dbg_owned_count(); i++) {
            const char *group = NULL, *label = dbg_owned_label(i, &group);
            if (!a1 || strstr(label, a1) || strstr(group, a1)) out_put("%3d %-9s %-22s %d\n", i, group, label, dbg_owned_get(i));
        }
    } else if (!strcmp(cmd, "give") && a1 && a2) {   // give INDEX 0|1: remove or give an owned item
        int i = atoi(a1);
        out_put("give %d %s: %s, now %d\n", i, a2, dbg_owned_set(i, atoi(a2)) ? "done" : "failed", dbg_owned_get(i));
    } else if (!strcmp(cmd, "fly") || !strcmp(cmd, "hover") || !strcmp(cmd, "hoverboots")) {   // fly|hover|hoverboots [0|1]
        if (a1) (cmd[0] == 'f' ? dbg_fly : !strcmp(cmd, "hover") ? dbg_hover : dbg_hover_boots)(atoi(a1));
        if (a2 && cmd[0] == 'f') dbg_set_fly_speed((float)atof(a2));   // fly 1 SPEED
        out_put("fly %d (%.1f m/s), hover %d, infinite hover boots %d\n", dbg_fly_on(), dbg_fly_speed(), dbg_hover_on(), dbg_hover_boots_on());
    } else if (!strcmp(cmd, "novoid")) {   // novoid [0|1]: no void-out
        if (a1) dbg_no_void_out(atoi(a1));
        out_put("novoid %d\n", dbg_no_void_out_on());
    } else if (!strcmp(cmd, "bombbug")) {   // bombbug [0|1]: every lifted bomb is held out to the side
        if (a1) dbg_bomb_bug(atoi(a1));
        out_put("bombbug %d\n", dbg_bomb_bug_on());
    } else if (!strcmp(cmd, "hook") || !strcmp(cmd, "walls")) {   // hook|walls [RANGE]: the hookshot or wall overlay's state and what is near
        static DbgHookPoly hp[4096];
        static DbgHookLabel hl[64];
        char st[160];
        dbg_hook_status(st, sizeof st);
        float range = a1 ? strtof(a1, NULL) : 30.0f;
        int walls = cmd[0] == 'w';
        int n = dbg_hook_polys(hp, 4096, range, walls), t = 0, m = walls ? 0 : dbg_hook_labels(hl, 64, range);
        for (int i = 0; i < n; i++) t += hp[i].target;
        out_put("%s\n%d polygons within %.0f m (%d of target objects)\n", st, n, range, t);
        for (int i = 0; i < n && i < 8; i++)
            out_put("  %s %.1f %.1f %.1f\n", walls ? "wall" : hp[i].target ? "target" : "surface", hp[i].p[0][0], hp[i].p[0][1],
                    hp[i].p[0][2]);
        for (int i = 0; i < m; i++)
            out_put("  object %s at %.1f %.1f %.1f%s\n", hl[i].name, hl[i].pos[0], hl[i].pos[1], hl[i].pos[2],
                    hl[i].shaped ? "" : " (no shape)");
    } else if (!strcmp(cmd, "saves")) {   // saves [new|make|switch NAME|load NAME|delete NAME|drop NAME]: profiles and manual saves (save_slots.c)
        static char names[SAVE_LIST_MAX][SAVE_NAME_MAX];
        char out[SAVE_NAME_MAX];
        // a name may have spaces: the rest of the line after the second word
        char *rest = NULL;
        if (a2) {
            rest = whole;
            for (int w = 0; w < 2; w++) {
                while (*rest == ' ' || *rest == '\t') rest++;
                while (*rest && *rest != ' ' && *rest != '\t') rest++;
            }
            while (*rest == ' ' || *rest == '\t') rest++;
            for (char *e = rest + strlen(rest); e > rest && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ');) *--e = 0;
        }
        if (a1 && !strcmp(a1, "new")) out_put("new: %s\n", save_profile_create(out) ? "failed" : out);
        else if (a1 && !strcmp(a1, "make")) {
            int r = save_manual_make(out);
            out_put("make: %s\n", r > 0 ? "no save yet" : r ? "failed" : out);
        } else if (a1 && !strcmp(a1, "switch") && rest) out_put("switch: %s (restart to apply)\n", save_profile_switch(rest) ? "failed" : "staged");
        else if (a1 && !strcmp(a1, "load") && rest) out_put("load: %s (restart to apply)\n", save_manual_load(rest) ? "failed" : "staged");
        else if (a1 && !strcmp(a1, "delete") && rest) out_put("delete profile: %s\n", save_profile_delete(rest) ? "failed" : "done");
        else if (a1 && !strcmp(a1, "drop") && rest) out_put("delete manual save: %s\n", save_manual_delete(rest) ? "failed" : "done");
        save_profile_active(out);
        int n = save_profile_list(names, SAVE_LIST_MAX);
        out_put("in use: %s; %d profiles:", out, n);
        for (int i = 0; i < n; i++) out_put(" [%s]", names[i]);
        n = save_manual_list(names, SAVE_LIST_MAX);
        out_put("\n%d manual saves:", n);
        for (int i = 0; i < n; i++) out_put(" [%s]", names[i]);
        out_put("\n");
    } else if (!strcmp(cmd, "enemies")) {   // enemies [RANGE]: the enemy health overlay's list (needs debug_view_health)
        static DbgHealth h[256];
        int n = dbg_view_health(h, 256, a1 ? (float)atof(a1) : 150.0f);
        out_put("%d enemies with life (kinds %d)\n", n, dbg_view_kinds());
        for (int i = 0; i < n; i++)
            out_put("  %-32s %d / %d at %.1f %.1f %.1f\n", h[i].name, h[i].life, h[i].max, h[i].pos[0], h[i].pos[1], h[i].pos[2]);
    } else if (!strcmp(cmd, "changes")) {   // changes [on|off|ignore|clear] [N]: the change monitor and its newest N lines
        if (a1 && !strcmp(a1, "on")) dbg_watch_set(1);
        else if (a1 && !strcmp(a1, "off")) dbg_watch_set(0);
        else if (a1 && !strcmp(a1, "ignore")) dbg_watch_ignore_changed();
        else if (a1 && !strcmp(a1, "clear")) dbg_watch_clear();
        int want = a2 ? atoi(a2) : a1 && isdigit((unsigned char)a1[0]) ? atoi(a1) : 40;
        int n = dbg_watch_count();
        out_put("changes %s: %s, %d lines, %d ignored, %d names\n", dbg_watch_on() ? "on" : "off", dbg_watch_state(), n,
                dbg_watch_ignored(), dbg_watch_names());
        char line[192];
        for (int i = (n < want ? n : want) - 1; i >= 0; i--)
            if (dbg_watch_line(i, line, sizeof line)) out_put("%s\n", line);
    } else if (!strcmp(cmd, "fog")) {   // fog [0|1]: 1 shows the volumetric fog, 0 removes it (NX2NAVI_DEBUG_FOG_OFF)
        if (a1) cfg_set("NX2NAVI_DEBUG_FOG_OFF", atoi(a1) ? "0" : "1");
        out_put("fog %s\n", cfg_bool("NX2NAVI_DEBUG_FOG_OFF") ? "off" : "on");
    } else if (!strcmp(cmd, "view")) {   // view [RANGE]: the volume overlay's list near the player (kinds as set)
        static DbgVolume v[256];
        static const char *const kind[] = {"", "wall", "load", "", "trigger", "", "", "", "hidden", "death"};
        int n = dbg_view_volumes(v, 256, a1 ? (float)atof(a1) : 60.0f);
        float vp[16], asp = 0;
        int cam = vk_dbgcam_vp(vp, &asp);
        out_put("%d volumes (kinds %d), camera %d aspect %.3f\n", n, dbg_view_kinds(), cam, asp);
        for (int i = 0; cam && i < 4; i++) out_put("  vp %8.4f %8.4f %8.4f %10.4f\n", vp[i * 4], vp[i * 4 + 1], vp[i * 4 + 2], vp[i * 4 + 3]);
        for (int i = 0; i < n; i++) {
            float c[4] = {0, 0, 0, 1};
            for (int r = 0; cam && r < 4; r++)
                c[r] = vp[r * 4] * v[i].pos[0] + vp[r * 4 + 1] * v[i].pos[1] + vp[r * 4 + 2] * v[i].pos[2] + vp[r * 4 + 3];
            out_put("%-7s %-40s pos %.1f %.1f %.1f scale %.2f %.2f %.2f ndc %.2f %.2f w %.1f%s%s\n", kind[v[i].kind == DBG_VOL_DEATH ? 9 : v[i].kind & 15],
                    v[i].name, v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].scale[0], v[i].scale[1], v[i].scale[2],
                    c[0] / c[3], c[1] / c[3], c[3], v[i].target[0] ? " > " : "", v[i].target);
        }
    } else if (!strcmp(cmd, "cfg") && a1) {   // cfg KEY [VALUE]: a setting (KEY without NX2NAVI_), set for this run
        char k[96];
        snprintf(k, sizeof k, "NX2NAVI_%s", a1);
        for (char *q = k; *q; q++) *q = (char)toupper((unsigned char)*q);
        if (a2) cfg_set(k, a2);
        const char *v = cfg_get(k);
        out_put("%s = %s\n", k, v ? v : "(unset)");
    } else if (!strcmp(cmd, "igt")) {   // igt [toggle|split|reset]: the speedrun timer and its splits
        if (a1 && !strcmp(a1, "toggle")) dbg_igt_toggle();
        if (a1 && !strcmp(a1, "split")) dbg_igt_split();
        if (a1 && !strcmp(a1, "reset")) out_put("reset%s\n", dbg_igt_reset() ? ", new best run" : "");
        uint64_t igt = 0, rta = 0;
        dbg_igt_times(&igt, &rta);
        out_put("igt %s %.3f s, rta %.3f s, shown %d, autosplit %d, best %d splits\n", dbg_igt_running() ? "running" : "stopped",
                igt / 1e9, rta / 1e9, dbg_igt_enabled(), dbg_igt_autosplit(), dbg_igt_best_count());
        for (int i = 0; i < dbg_igt_splits(); i++) {
            uint64_t ns;
            int64_t d;
            int hd;
            char name[48];
            if (dbg_igt_split_at(i, &ns, name, &d, &hd)) out_put("%2d %-32s %.3f%s%+.3f\n", i + 1, name, ns / 1e9, hd ? " " : " (no best) ", d / 1e9);
        }
    } else if (!strcmp(cmd, "magiclvl")) {   // magiclvl [0|1|2]: the magic meter's level
        if (a1) dbg_magic_set_level(atoi(a1));
        out_put("magic level %d\n", dbg_magic_level());
    } else if (!strcmp(cmd, "agesw")) {   // agesw: switch between child and adult in this area
        out_put("age switch to warp %d: %s\n%s\n", dbg_age_switch_target(), dbg_age_switch() ? "started" : "failed", dbg_warp_note());
    } else if (!strcmp(cmd, "reload")) {   // reload: this area again from the entrance the player came in by
        out_put("reload to warp %d: %s\n%s\n", dbg_reload_target(), dbg_reload_area() ? "started" : "failed", dbg_warp_note());
    } else if (!strcmp(cmd, "warpage")) {   // warpage [area|child|adult]: the age flag held after a warp
        if (a1) dbg_warp_set_age(!strcmp(a1, "child") ? DBG_AGE_CHILD : !strcmp(a1, "adult") ? DBG_AGE_ADULT : DBG_AGE_ANY);
        int age = dbg_warp_get_age();
        out_put("warp age: %s\n", age == DBG_AGE_CHILD ? "child" : age == DBG_AGE_ADULT ? "adult" : "the area's");
    } else if (!strcmp(cmd, "area")) {
        char s[64] = "";
        dbg_current_area(s);
        out_put("area %s, child %d, warp armed %d, %s\n", s, dbg_is_child(), dbg_warp_armed(), dbg_warp_note());
    } else if (!strcmp(cmd, "perf")) {   // the player actor and its event performer
        out_put("player 0x%llx performer 0x%llx\n", (unsigned long long)dbg_player_actor(), (unsigned long long)dbg_player_performer());
    } else if (!strcmp(cmd, "gcall") && a1) {
        // gcall FN [ARG ...]: call a guest function on the game thread at the next frame. An argument is a number,
        // s:TEXT (the address of TEXT), p:TEXT (the address of a pointer to TEXT), player or perf.
        uint64_t a[8] = {0};
        char *args[8] = {a2, a3};
        int n = (a2 != NULL) + (a3 != NULL), slot = 0;
        char *t;
        while (n < 8 && (t = strtok(NULL, " \t\r\n"))) args[n++] = t;
        for (int i = 0; i < n; i++) {
            if (!strncmp(args[i], "s:", 2) || !strncmp(args[i], "p:", 2)) a[i] = dbg_guest_text(slot++ & 3, args[i] + 2, args[i][0] == 'p');
            else if (!strcmp(args[i], "player")) a[i] = dbg_player_actor();
            else if (!strcmp(args[i], "perf")) a[i] = dbg_player_performer();
            else if (!strncmp(args[i], "buf", 3)) a[i] = dbg_guest_scratch() + num(args[i] + 3);
            else a[i] = num(args[i]);
        }
        out_put("gcall 0x%llx: %s\n", (unsigned long long)num(a1), dbg_guest_call(num(a1), a) ? "queued" : "busy");
        for (int i = 0; i < 50; i++) {
            uint64_t r;
            if (dbg_guest_result(&r)) { out_put("result 0x%llx\n", (unsigned long long)r); break; }
            plat_sleep_ns(20000000ull);
        }
    } else if (!strcmp(cmd, "pad")) {
        // pad clear; pad hold BUTTONS LX LY MS; pad wait ADDR VALUE BUTTONS LX LY MAXMS (hold until the byte at ADDR
        // reads VALUE); pad run; pad: the state. BUTTONS are Switch npad bits in hex (1 A, 2 B, 200 ZR, 1000 D-pad left).
        if (a1 && !strcmp(a1, "clear")) {
            dbg_pad_clear();
            out_put("pad cleared\n");
        } else if (a1 && !strcmp(a1, "run")) {
            out_put(dbg_pad_run() ? "pad running\n" : "pad: no steps\n");
        } else if (a1 && (!strcmp(a1, "hold") || !strcmp(a1, "wait"))) {
            char *v[6] = {a2, a3, NULL, NULL, NULL, NULL};
            for (int i = 2; i < 6; i++) v[i] = strtok(NULL, " \t\r\n");
            int ok;
            if (a1[0] == 'h')
                ok = v[3] && dbg_pad_add(num(v[0]), (int32_t)strtol(v[1], NULL, 0), (int32_t)strtol(v[2], NULL, 0), num(v[3]), 0, 0);
            else
                ok = v[5] && dbg_pad_add(num(v[2]), (int32_t)strtol(v[3], NULL, 0), (int32_t)strtol(v[4], NULL, 0), num(v[5]),
                                         num(v[0]), (uint8_t)num(v[1]));
            out_put(ok ? "pad step added\n" : "pad: bad step, too many steps or a script running\n");
        } else {
            int step, total;
            uint64_t w[64];
            int on = dbg_pad_state(&step, &total, w, 64);
            out_put("pad %s, step %d of %d; waits held (ms):", on ? "running" : "idle", step, total);
            for (int i = 0; i < total && i < 64; i++) out_put(" %llu", (unsigned long long)w[i]);
            out_put("\n");
        }
    } else if (!strcmp(cmd, "clip")) {   // clip: save a clip now, as the Save clip hotkey does
        out_put(clip_save() ? "clip saving\n" : "clip: not recording or busy\n");
    } else if (!strcmp(cmd, "snap")) {
        // snap FRAMES ADDR LEN [ADDR LEN ...]: record the ranges on every frame into a ring of FRAMES; snap save FILE:
        // the kept frames to a file next to the game; snap off; snap: the state.
        if (a1 && !strcmp(a1, "off")) {
            dbg_snap_stop();
            out_put("snap off, %u frames kept\n", dbg_snap_frames());
        } else if (a1 && !strcmp(a1, "save") && a2) {
            uint32_t n = dbg_snap_frames();
            out_put(dbg_snap_save(a2) ? "saved %u frames to %s\n" : "cannot save (%u frames) to %s\n", n, a2);
        } else if (a1 && a2 && a3) {
            uint64_t ad[16], ln[16];
            int n = 0;
            char *t;
            ad[n] = num(a2), ln[n++] = num(a3);
            while (n < 16 && (t = strtok(NULL, " \t\r\n"))) {
                char *l = strtok(NULL, " \t\r\n");
                if (!l) break;
                ad[n] = num(t), ln[n++] = num(l);
            }
            out_put(dbg_snap_start(ad, ln, n, (uint32_t)num(a1)) ? "recording %d range(s), %s frames\n"
                                                                 : "refused: %d range(s), %s frames (128 KiB a frame, 512 MiB in all)\n",
                    n, a1);
        } else {
            out_put("snap %s, %u frames kept\n", dbg_snap_on() ? "on" : "off", dbg_snap_frames());
        }
    } else if (!strcmp(cmd, "ftrace")) {
        // ftrace ADDR [ADDR ...]: log every entry of these guest functions (up to 16) with the caller; ftrace: the log;
        // ftrace off. The caller is the return address, inside the calling function.
        if (a1 && !strcmp(a1, "off")) {
            dbg_fntrace_off();
            out_put("tracing off\n");
            return;
        }
        if (a1) {
            uint64_t w[16];
            int n = 0;
            char *t;
            w[n++] = num(a1);
            if (a2) w[n++] = num(a2);
            if (a3) w[n++] = num(a3);
            while (n < 16 && (t = strtok(NULL, " \t\r\n"))) w[n++] = num(t);
            dbg_fntrace_watch(w, n);
            out_put("watching %d function(s)\n", n);
            return;
        }
        uint64_t addr, lr, x0, ns, t0 = 0;
        char d[128];
        for (uint64_t i = 0; dbg_fntrace_ring(i, &addr, &lr, &x0, &ns); i++) {
            if (!t0) t0 = ns;
            out_put("%8.3f 0x%llx from %s x0 0x%llx\n", (ns - t0) / 1e9, (unsigned long long)addr, ldr_describe(lr, d, sizeof d),
                    (unsigned long long)x0);
        }
    } else if (!strcmp(cmd, "fcount")) {
        // fcount start; fcount LO [HI]: the functions entered LO to HI times since the start (default HI = LO);
        // fcount off.
        if (a1 && !strcmp(a1, "start")) {
            out_put("%s\n", dbg_fntrace_count_start() ? "counting" : "no memory");
            return;
        }
        if (a1 && !strcmp(a1, "off")) {
            dbg_fntrace_off();
            out_put("counting off\n");
            return;
        }
        uint32_t lo = a1 ? (uint32_t)num(a1) : 1, hi = a2 ? (uint32_t)num(a2) : lo, distinct = 0, dropped = 0;
        int n = dbg_fntrace_counts(lo, hi, fcount_line, NULL, &distinct, &dropped);
        out_put("%d function(s) entered %u to %u times; %u seen, %u entries dropped\n", n, lo, hi, distinct, dropped);
    } else if (!strcmp(cmd, "gbuf")) {   // the scratch buffer's address (gcall takes bufOFFSET for an address in it)
        out_put("gbuf 0x%llx\n", (unsigned long long)dbg_guest_scratch());
    } else if (!strcmp(cmd, "gres")) {
        uint64_t r;
        if (dbg_guest_result(&r)) out_put("result 0x%llx\n", (unsigned long long)r);
        else out_put("no result yet\n");
    } else {
        out_put("unknown or incomplete command. Commands: overlay hotkeys hexec hwatch hunwatch anchor saves watch unwatch wlog dump vtable main regions read ptrs chain str write find findptr scan scanf next hits set player teleport speed health sethealth setmaxhealth god onehit lank lock flag fget fset items item magic infmagic infitems warps warp warpcancel agesw reload warpage own give magiclvl fly hover hoverboots novoid bombbug igt fog view cfg area perf gcall gres ftrace fcount snap pad clip\n");
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

void dbg_remote_set(int on) {   // kept as debug_remote (debug_tools.c dbg_keep)
    enabled = on != 0;
    dbg_keep("NX2NAVI_DEBUG_REMOTE", on ? "1" : "0");
    if (on && !started) {
        started = 1;
        plat_thread_spawn(remote_thread, NULL, "debug-remote", 0);
    }
}

int dbg_remote_enabled(void) { return enabled; }
