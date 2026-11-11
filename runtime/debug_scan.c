// Live memory scanner (debug_scan.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_scan.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define SCAN_CAP (1u << 21)

static uint64_t *hit_addr;
static int32_t *hit_val;
static uint32_t hits;
static int truncated;

static int ensure(void) {
    if (!hit_addr) hit_addr = malloc(SCAN_CAP * sizeof *hit_addr);
    if (!hit_val) hit_val = malloc(SCAN_CAP * sizeof *hit_val);
    return hit_addr && hit_val;
}

void dbg_scan_reset(void) { hits = 0; truncated = 0; }
int dbg_scan_count(void) { return (int)hits; }
int dbg_scan_truncated(void) { return truncated; }

int dbg_scan_first(int32_t value) {
    if (!ensure()) return 0;
    hits = 0;
    truncated = 0;
    uint64_t addr = 0x10000;
    while (addr < (1ull << 47)) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, 1ull << 47, &end);
        if (end <= addr) break;
        if (st == PLAT_PROT_RW) {
            enum { CHUNK = 1 << 20 };
            static int32_t buf[CHUNK / 4];
            for (uint64_t at = addr; at < end; at += CHUNK) {
                uint64_t n = (end - at < CHUNK ? end - at : CHUNK) / 4;
                if (!plat_mem_read(at, buf, n * 4)) continue;   // never read pages directly: GPU-mapped ones can fault
                for (uint64_t i = 0; i < n; i++)
                    if (buf[i] == value) {
                        if (hits >= SCAN_CAP) { truncated = 1; return (int)hits; }
                        hit_addr[hits] = at + i * 4;
                        hit_val[hits++] = value;
                    }
            }
        }
        addr = end;
    }
    return (int)hits;
}

int dbg_scan_next(int mode, int32_t value) {
    uint32_t keep = 0;
    for (uint32_t i = 0; i < hits; i++) {
        int32_t now;
        if (!plat_mem_read(hit_addr[i], &now, 4)) continue;
        int ok;
        switch (mode) {
        case SCAN_EQUAL: ok = now == value; break;
        case SCAN_CHANGED: ok = now != hit_val[i]; break;
        case SCAN_UNCHANGED: ok = now == hit_val[i]; break;
        case SCAN_INCREASED: ok = now > hit_val[i]; break;
        default: ok = now < hit_val[i]; break;
        }
        if (ok) {
            hit_addr[keep] = hit_addr[i];
            hit_val[keep++] = now;
        }
    }
    hits = keep;
    return (int)hits;
}

int dbg_scan_get(int i, uint64_t *addr, int32_t *now) {
    if (i < 0 || (uint32_t)i >= hits) return 0;
    *addr = hit_addr[i];
    if (!plat_mem_read(hit_addr[i], now, 4)) *now = 0;
    return 1;
}

int dbg_scan_write(int i, int32_t value) {
    if (i < 0 || (uint32_t)i >= hits) return 0;
    uint64_t at = hit_addr[i], end;
    if (plat_mem_state(at & ~0xFFFull, (at & ~0xFFFull) + 0x2000, &end) != PLAT_PROT_RW) return 0;
    plat_mem_prepare_write(at, 4);
    memcpy((void *)(uintptr_t)at, &value, 4);
    return 1;
}

int dbg_scan_dump(void) {
    FILE *f = fopen("debug_scan.txt", "w");
    if (!f) return 0;
    fprintf(f, "main base 0x%llx, %u hits%s\n", (unsigned long long)MOD_MAIN_BASE, hits, truncated ? " (truncated)" : "");
    for (uint32_t i = 0; i < hits && i < 4096; i++) {
        int32_t now = 0;
        plat_mem_read(hit_addr[i], &now, 4);
        uint64_t a = hit_addr[i];
        fprintf(f, "0x%llx  main%+lld  value %d (0x%x)\n", (unsigned long long)a, (long long)(a - MOD_MAIN_BASE), now, (unsigned)now);
    }
    fclose(f);
    return 1;
}
