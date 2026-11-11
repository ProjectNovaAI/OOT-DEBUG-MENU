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
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    uint64_t n = e - a - c->len + 1;
    for (uint64_t i = 0; i < n && c->found < MAX_FIND; i += c->align)
        if (p[i] == c->pat[0] && !memcmp(p + i, c->pat, c->len)) {
            out_put("0x%llx (main%+lld)\n", (unsigned long long)(a + i), (long long)(a + i - MOD_MAIN_BASE));
            c->found++;
        }
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
        out_put("unknown or incomplete command. Commands: main regions read ptrs chain str write find findptr scan scanf next hits set player teleport speed\n");
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
        plat_sleep_ns(50ull * 1000 * 1000);
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
