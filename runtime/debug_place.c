// The current area's placement files for the debug menu's volume overlay (debug_view.c): some objects the overlay needs
// are never live actors (the lava surfaces and the black pit bottoms are passive placements the game merges into the
// level), so they are read from the stage's own files instead: Banc/<area>/<banc>_Static.bcett.byml.zs and
// _Dynamic.bcett.byml.zs, zstd frames made with the bcett.byml.zsdic dictionary of Pack/ZsDic.pack.zs, holding a BYML
// (version 7, little endian) whose root dictionary has an "Actors" array of dictionaries: Gyaml (the actor's name),
// Translate, Rotate (radians, applied as Ry * Rx * Rz: checked against the live axes of rotated Area actors) and Scale.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "debug_tools.h"
#include "fs.h"
#include "host.h"

#define MAX_RAW (64u << 20)

static int u32_at(const uint8_t *p, size_t n, size_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

// SARC member `name` of the archive p[0..n): its start and size; 0 when it is not there.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    uint32_t data;
    uint16_t head, count, bom;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return 0;
    memcpy(&head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    if (bom != 0xfeff || !u32_at(p, n, 12, &data) || head + 12u > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = head + 12 + (size_t)count * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return 0;
    names += 8;
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = head + 12 + (size_t)i * 16;
        if (!u32_at(p, n, e + 4, &attr) || !u32_at(p, n, e + 8, &lo) || !u32_at(p, n, e + 12, &hi)) return 0;
        size_t s = names + (size_t)(attr & 0xffffff) * 4, len = strlen(name);
        if (!(attr >> 24) || s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || data + (size_t)hi > n) return 0;
        *at = data + lo;
        *size = hi - lo;
        return 1;
    }
    return 0;
}

// The stage files' dictionary, made once.
static ZSTD_DDict *stage_dict(void) {
    static ZSTD_DDict *dd;
    static int tried;
    if (tried) return dd;
    tried = 1;
    uint64_t zn = 0;
    uint8_t *z = romfs_load("Pack/ZsDic.pack.zs", &zn), *dics = NULL;
    unsigned long long dn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : ZSTD_CONTENTSIZE_ERROR;
    size_t at, size;
    if (dn != ZSTD_CONTENTSIZE_UNKNOWN && dn != ZSTD_CONTENTSIZE_ERROR && dn <= MAX_RAW && (dics = malloc((size_t)dn)) &&
        ZSTD_decompress(dics, (size_t)dn, z, (size_t)zn) == dn &&
        sarc_member(dics, (size_t)dn, "bcett.byml.zsdic", &at, &size))
        dd = ZSTD_createDDict(dics + at, size);
    free(dics);
    free(z);
    return dd;
}

// The romfs-relative path of the file whose name is `name`, searched over the index (the area's folder is not always
// the banc name without its age).
static const char *find_file(const char *name) {
    size_t ln = strlen(name);
    for (uint64_t i = 0, n = romfs_file_count(); i < n; i++) {
        uint64_t off, sz;
        const char *p = romfs_entry(i, &off, &sz);
        if (!p) continue;
        size_t lp = strlen(p);
        if (lp > ln && p[lp - ln - 1] == '/' && !memcmp(p + lp - ln, name, ln) && !strncmp(p, "Banc/", 5)) return p;
    }
    return NULL;
}

static uint8_t *load_stage(const char *file, size_t *n_out) {
    const char *path = find_file(file);
    ZSTD_DDict *dd = stage_dict();
    if (!path || !dd) return NULL;
    uint64_t zn = 0;
    uint8_t *z = romfs_load(path, &zn), *raw = NULL;
    unsigned long long rn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : ZSTD_CONTENTSIZE_ERROR;
    if (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn <= MAX_RAW && (raw = malloc((size_t)rn))) {
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        size_t got = dc ? ZSTD_decompress_usingDDict(dc, raw, (size_t)rn, z, (size_t)zn, dd) : 0;
        ZSTD_freeDCtx(dc);
        if (ZSTD_isError(got) || got != rn) free(raw), raw = NULL;
    }
    free(z);
    *n_out = (size_t)rn;
    return raw;
}

// ---- BYML: dictionaries (0xc1: 8-byte entries {key index:24, type:8, value}), arrays (0xc0: types, padded to 4, then
// values), strings (0xa0: index into the string table), floats (0xd2).
typedef struct {
    const uint8_t *p;
    size_t n;
    uint32_t keys, strs;
} Byml;

static const char *table_str(const Byml *b, uint32_t table, uint32_t i) {
    uint32_t head, off;
    if (!table || !u32_at(b->p, b->n, table, &head) || i >= head >> 8 || !u32_at(b->p, b->n, table + 4 + 4 * i, &off))
        return NULL;
    size_t s = (size_t)table + off;
    if (s >= b->n || !memchr(b->p + s, 0, b->n - s)) return NULL;
    return (const char *)b->p + s;
}

// The value and type of `key` in the dictionary at node; 0 when it is not there.
static int dict_get(const Byml *b, uint32_t node, const char *key, uint8_t *type, uint32_t *value) {
    uint32_t head;
    if (!u32_at(b->p, b->n, node, &head) || (head & 0xff) != 0xc1) return 0;
    for (uint32_t i = 0, cnt = head >> 8; i < cnt; i++) {
        uint32_t e, v;
        size_t at = (size_t)node + 4 + 8 * (size_t)i;
        if (!u32_at(b->p, b->n, at, &e) || !u32_at(b->p, b->n, at + 4, &v)) return 0;
        const char *k = table_str(b, b->keys, e & 0xffffff);
        if (k && !strcmp(k, key)) {
            *type = (uint8_t)(e >> 24), *value = v;
            return 1;
        }
    }
    return 0;
}

// Array element i at node: its type and value; 0 past the end.
static int array_get(const Byml *b, uint32_t node, uint32_t i, uint8_t *type, uint32_t *value) {
    uint32_t head;
    if (!u32_at(b->p, b->n, node, &head) || (head & 0xff) != 0xc0 || i >= head >> 8) return 0;
    uint32_t cnt = head >> 8;
    size_t types = (size_t)node + 4, values = types + ((cnt + 3u) & ~3u);
    if (types + cnt > b->n || !u32_at(b->p, b->n, values + 4 * (size_t)i, value)) return 0;
    *type = b->p[types + i];
    return 1;
}

// Three floats from `key` (an array of floats, or one float for all three); 0 when it is not there.
static int vec3(const Byml *b, uint32_t node, const char *key, float out[3]) {
    uint8_t t;
    uint32_t v;
    if (!dict_get(b, node, key, &t, &v)) return 0;
    if (t == 0xd2) {
        memcpy(&out[0], &v, 4);
        out[1] = out[2] = out[0];
        return 1;
    }
    if (t != 0xc0) return 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t et;
        uint32_t ev;
        if (!array_get(b, v, i, &et, &ev) || et != 0xd2) return 0;
        memcpy(&out[i], &ev, 4);
    }
    return 1;
}

// The rotation's three unit columns (x, y, z axes) for Euler angles r applied as Ry * Rx * Rz.
static void axes_of(const float r[3], float a[9]) {
    float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
    // M = Ry Rx Rz, row-major m[row][col]
    float m[3][3] = {
        {cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx},
        {cx * sz, cx * cz, -sx},
        {-sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx},
    };
    for (int c = 0; c < 3; c++)
        for (int r2 = 0; r2 < 3; r2++) a[c * 3 + r2] = m[r2][c];
}

static int each_actor(const uint8_t *p, size_t n, int (*fn)(const DbgPlacement *, void *), void *ctx) {
    Byml b = {p, n, 0, 0};
    uint32_t root;
    uint16_t ver;
    if (n < 16 || memcmp(p, "YB", 2)) return 0;
    memcpy(&ver, p + 2, 2);
    if (ver < 2 || !u32_at(p, n, 4, &b.keys) || !u32_at(p, n, 8, &b.strs) || !u32_at(p, n, 12, &root)) return 0;
    uint8_t t;
    uint32_t actors;
    if (!dict_get(&b, root, "Actors", &t, &actors) || t != 0xc0) return 0;
    int count = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t et, gt;
        uint32_t node, gv;
        if (!array_get(&b, actors, i, &et, &node)) break;
        if (et != 0xc1 || !dict_get(&b, node, "Gyaml", &gt, &gv) || gt != 0xa0) continue;
        const char *name = table_str(&b, b.strs, gv);
        DbgPlacement pl;
        static const float zero[3] = {0, 0, 0};
        if (!name || !vec3(&b, node, "Translate", pl.pos)) continue;
        float rot[3];
        if (!vec3(&b, node, "Rotate", rot)) memcpy(rot, zero, sizeof rot);
        if (!vec3(&b, node, "Scale", pl.scale)) pl.scale[0] = pl.scale[1] = pl.scale[2] = 1.0f;
        axes_of(rot, pl.axes);
        pl.name = name;
        count++;
        if (!fn(&pl, ctx)) break;
    }
    return count;
}

int dbg_place_each(const char *banc, int (*fn)(const DbgPlacement *, void *), void *ctx) {
    static const char *const parts[] = {"_Static.bcett.byml.zs", "_Dynamic.bcett.byml.zs"};
    int total = 0, found = 0;
    for (int k = 0; k < 2; k++) {
        char file[128];
        size_t n = 0;
        snprintf(file, sizeof file, "%s%s", banc, parts[k]);
        uint8_t *raw = load_stage(file, &n);
        if (!raw) continue;
        found = 1;
        total += each_actor(raw, n, fn, ctx);
        free(raw);
    }
    return found ? total : -1;
}
