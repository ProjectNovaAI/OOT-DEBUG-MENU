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
#include "platform/platform.h"

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

// A dictionary of Pack/ZsDic.pack.zs by its member name ("bcett.byml.zsdic" for the stage files, "zs.zsdic" for the
// collision files, "pack.zsdic" for the actor packs), made once each; NULL when it is not there.
static ZSTD_DDict *zs_dict(const char *member) {
    static const char *const names[] = {"bcett.byml.zsdic", "zs.zsdic", "pack.zsdic"};
    static ZSTD_DDict *dd[3];
    static int tried[3];
    static PlatMutex lock = PLAT_MUTEX_INIT;
    int k = 0;
    while (k < 3 && strcmp(member, names[k])) k++;
    if (k == 3) return NULL;
    plat_mutex_lock(&lock);
    if (!tried[k]) {
        tried[k] = 1;
        uint64_t zn = 0;
        uint8_t *z = romfs_load("Pack/ZsDic.pack.zs", &zn), *dics = NULL;
        unsigned long long dn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : ZSTD_CONTENTSIZE_ERROR;
        size_t at, size;
        if (dn != ZSTD_CONTENTSIZE_UNKNOWN && dn != ZSTD_CONTENTSIZE_ERROR && dn <= MAX_RAW &&
            (dics = malloc((size_t)dn)) && ZSTD_decompress(dics, (size_t)dn, z, (size_t)zn) == dn &&
            sarc_member(dics, (size_t)dn, member, &at, &size))
            dd[k] = ZSTD_createDDict(dics + at, size);
        free(dics);
        free(z);
    }
    plat_mutex_unlock(&lock);
    return dd[k];
}

uint8_t *dbg_romfs_unzs(const char *path, const char *dict, size_t *n_out) {
    ZSTD_DDict *dd = zs_dict(dict);
    *n_out = 0;
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
    if (raw) *n_out = (size_t)rn;
    return raw;
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
    return dbg_romfs_unzs(find_file(file), "bcett.byml.zsdic", n_out);
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

// A 64-bit integer value (0xd4, 0xd5: an offset to its 8 bytes) of `key` in the dictionary at node.
static int u64_get(const Byml *b, uint32_t node, const char *key, uint64_t *out) {
    uint8_t t;
    uint32_t v;
    if (!dict_get(b, node, key, &t, &v) || (t != 0xd4 && t != 0xd5) || v > b->n || b->n - v < 8) return 0;
    memcpy(out, b->p + v, 8);
    return 1;
}

// The logic modules whose Area makes touching it a void-out: the fall-down gimmicks (Gimmick_AreaContact_FallDown,
// _FallDownMountian, _FallDown_GanonCastle) and the two level modules that do the same with one Area of their own. The
// _FallDown_Item modules only put dropped items back, so they are left out. Each module was checked to hold the
// HoldSensorHitInfoGiveAbyssActor node (game 1.1.1).
static int void_logic(const char *logic) {
    const char *base = strrchr(logic, '/');
    base = base ? base + 1 : logic;
    if (strstr(base, "FallDown")) return !strstr(base, "FallDown_Item");
    return !strncmp(base, "Market_Child_Prefab_d415.", 25) || !strncmp(base, "CrumblingGanonCastle_Prefab_633f.", 33);
}

// The hashes of the Area placements a void-out logic module refers to: the root's AiGroups list dictionaries {Logic (the
// module's path), References [{Id, Path, Reference: the placement's Hash}]}. malloc'd, *n their count.
static uint64_t *void_hashes(const Byml *b, uint32_t root, int *n_out) {
    uint8_t t;
    uint32_t groups;
    uint64_t *out = NULL;
    int n = 0, cap = 0;
    *n_out = 0;
    if (!dict_get(b, root, "AiGroups", &t, &groups) || t != 0xc0) return NULL;
    for (uint32_t i = 0;; i++) {
        uint8_t gt, lt, rt;
        uint32_t g, lv, refs;
        if (!array_get(b, groups, i, &gt, &g)) break;
        if (gt != 0xc1 || !dict_get(b, g, "Logic", &lt, &lv) || lt != 0xa0) continue;
        const char *logic = table_str(b, b->strs, lv);
        if (!logic || !void_logic(logic) || !dict_get(b, g, "References", &rt, &refs) || rt != 0xc0) continue;
        for (uint32_t j = 0;; j++) {
            uint8_t et, it;
            uint32_t r, iv;
            uint64_t h;
            if (!array_get(b, refs, j, &et, &r)) break;
            if (et != 0xc1 || !dict_get(b, r, "Id", &it, &iv) || it != 0xa0) continue;
            const char *id = table_str(b, b->strs, iv);
            if (!id || strncmp(id, "Area_", 5) || !u64_get(b, r, "Reference", &h)) continue;
            if (n == cap) {
                uint64_t *v = realloc(out, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *v);
                if (!v) break;
                out = v;
            }
            out[n++] = h;
        }
    }
    *n_out = n;
    return out;
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
    int count = 0, nvoid = 0;
    uint64_t *voids = void_hashes(&b, root, &nvoid);
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
        pl.void_out = 0;
        uint64_t h;
        if (nvoid && !strcmp(name, "Area") && u64_get(&b, node, "Hash", &h))
            for (int k = 0; k < nvoid && !pl.void_out; k++) pl.void_out = voids[k] == h;
        count++;
        if (!fn(&pl, ctx)) break;
    }
    free(voids);
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

// ---- the parts of an actor the hookshot sticks to (dbg_actor_stick_shapes)
// An actor's pack, Pack/Actor/<name>.pack.zs (zstd with pack.zsdic), is a SARC of BYML files. Its physics parameter
// (Component/Physics/<name>.engine__component__PhysicsParam.bgyml) names a controller set (ControllerSetPath), whose
// ShapeNamePathAry lists the shape parameters (Phive/ShapeParam/*.phive__ShapeParam.bgyml). A file that lacks a key
// takes it from its "$parent". Paths are written "Work/<the member without .bgyml>.gyml". A shape parameter holds lists
// of Box (Center, HalfExtents), Polytope (Vertices), Cylinder (CenterA, CenterB, Radius), Sphere, Capsule and PhshMesh
// (a mesh file) entries, each with MaterialPresets and an offset (OffsetTranslation, OffsetRotation in degrees); a
// vector is a dictionary {X, Y, Z}. The entries with the preset Wall_StickHookshot are what the hookshot sticks to: the
// plate of Obj_HookShotTarget_A_01, not its rim (Physical_Edge, a mesh without the preset). No mesh entry of a target
// object has the preset and no target uses a sphere or a capsule (game 1.1.1), so those are not read.
static int byml_open(const uint8_t *p, size_t n, Byml *b, uint32_t *root) {
    uint16_t ver;
    b->p = p, b->n = n;
    if (n < 16 || memcmp(p, "YB", 2)) return 0;
    memcpy(&ver, p + 2, 2);
    return ver >= 2 && u32_at(p, n, 4, &b->keys) && u32_at(p, n, 8, &b->strs) && u32_at(p, n, 12, root);
}

// The pack member a "Work/....gyml" path names.
static int work_member(const char *work, char *out, size_t n) {
    if (!strncmp(work, "Work/", 5)) work += 5;
    size_t k = strlen(work);
    if (k < 5 || strcmp(work + k - 5, ".gyml") || k + 2 > n) return 0;
    snprintf(out, n, "%.*s.bgyml", (int)(k - 5), work);
    return 1;
}

// key in pack member `member`, or in its parents when it is missing there: the BYML it is in (b), its type and value.
static int pack_key(const uint8_t *pk, size_t pn, const char *member, const char *key, Byml *b, uint8_t *t, uint32_t *v) {
    char m[192];
    snprintf(m, sizeof m, "%s", member);
    for (int depth = 0; depth < 8; depth++) {
        size_t at, size;
        uint32_t root, pv;
        uint8_t pt;
        if (!sarc_member(pk, pn, m, &at, &size) || !byml_open(pk + at, size, b, &root)) return 0;
        if (dict_get(b, root, key, t, v)) return 1;
        const char *parent;
        if (!dict_get(b, root, "$parent", &pt, &pv) || pt != 0xa0 || !(parent = table_str(b, b->strs, pv)) ||
            !work_member(parent, m, sizeof m))
            return 0;
    }
    return 0;
}

static float f32_of(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// The {X, Y, Z} dictionary node as a vector (a missing component is 0).
static void xyz_node(const Byml *b, uint32_t node, float out[3]) {
    static const char *const c[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        uint8_t ct;
        uint32_t cv;
        out[i] = dict_get(b, node, c[i], &ct, &cv) && ct == 0xd2 ? f32_of(cv) : 0;
    }
}

// The vector at key of node; 0 (and a zero vector) when it is not there.
static int xyz(const Byml *b, uint32_t node, const char *key, float out[3]) {
    uint8_t t;
    uint32_t v;
    out[0] = out[1] = out[2] = 0;
    if (!dict_get(b, node, key, &t, &v) || t != 0xc1) return 0;
    xyz_node(b, v, out);
    return 1;
}

static int sticks(const Byml *b, uint32_t node) {
    uint8_t t;
    uint32_t arr;
    if (!dict_get(b, node, "MaterialPresets", &t, &arr) || t != 0xc0) return 0;
    for (uint32_t i = 0;; i++) {
        uint8_t et;
        uint32_t ev;
        if (!array_get(b, arr, i, &et, &ev)) return 0;
        const char *s = et == 0xa0 ? table_str(b, b->strs, ev) : NULL;
        if (s && strstr(s, "StickHookshot")) return 1;
    }
}

typedef struct {
    float axes[9], t[3];   // the entry's offset: rotation columns, translation
    int (*fn)(const float (*pts)[3], int n, void *ctx);
    void *ctx;
    int stop;
} StickOut;

static void stick_emit(StickOut *o, const float (*pts)[3], int n) {
    if (o->stop || n < 3) return;
    float w[8][3];
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++)
        for (int r = 0; r < 3; r++)
            w[i][r] = o->t[r] + o->axes[r] * pts[i][0] + o->axes[3 + r] * pts[i][1] + o->axes[6 + r] * pts[i][2];
    if (!o->fn((const float (*)[3])w, n, o->ctx)) o->stop = 1;
}

static void stick_box(StickOut *o, const float c[3], const float h[3]) {
    static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    float v[8][3];
    for (int i = 0; i < 8; i++)
        for (int a = 0; a < 3; a++) v[i][a] = c[a] + ((i >> a & 1) ? h[a] : -h[a]);
    for (int f = 0; f < 6; f++) {
        float q[4][3];
        for (int k = 0; k < 4; k++) memcpy(q[k], v[faces[f][k]], sizeof q[k]);
        stick_emit(o, (const float (*)[3])q, 4);
    }
}

static void sub3(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// The faces of the convex hull of v[0..n): each plane through three of the points with all the points on one side,
// its points in order round their middle. A face of more than 8 corners goes out as a fan of pieces of at most 8.
#define HULL_MAX 64
static void stick_hull(StickOut *o, const float (*v)[3], int n) {
    static float planes[HULL_MAX * 4][4];
    int np = 0;
    const float eps = 1e-4f;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            for (int k = j + 1; k < n && np < HULL_MAX * 4 && !o->stop; k++) {
                float e1[3], e2[3], nm[3];
                sub3(v[j], v[i], e1), sub3(v[k], v[i], e2), cross3(e1, e2, nm);
                float len = sqrtf(dot3(nm, nm));
                if (len < 1e-8f) continue;
                for (int a = 0; a < 3; a++) nm[a] /= len;
                float d = dot3(nm, v[i]);
                int above = 0, below = 0;
                for (int p = 0; p < n; p++) {
                    float s = dot3(nm, v[p]) - d;
                    above |= s > eps, below |= s < -eps;
                }
                if (above && below) continue;
                if (above) nm[0] = -nm[0], nm[1] = -nm[1], nm[2] = -nm[2], d = -d;   // the normal points out
                int seen = 0;
                for (int q = 0; q < np && !seen; q++) seen = dot3(planes[q], nm) > 0.9999f && fabsf(planes[q][3] - d) < eps;
                if (seen) continue;
                memcpy(planes[np], nm, sizeof nm), planes[np][3] = d;
                np++;
                int idx[HULL_MAX], m = 0;
                float mid[3] = {0, 0, 0};
                for (int p = 0; p < n; p++)
                    if (fabsf(dot3(nm, v[p]) - d) <= eps) idx[m++] = p;
                for (int p = 0; p < m; p++)
                    for (int a = 0; a < 3; a++) mid[a] += v[idx[p]][a] / (float)m;
                float u[3], w[3], ang[HULL_MAX];
                sub3(v[idx[0]], mid, u);
                float ul = sqrtf(dot3(u, u));
                if (ul < 1e-8f) continue;
                for (int a = 0; a < 3; a++) u[a] /= ul;
                cross3(nm, u, w);
                for (int p = 0; p < m; p++) {
                    float r[3];
                    sub3(v[idx[p]], mid, r);
                    ang[p] = atan2f(dot3(r, w), dot3(r, u));
                }
                for (int p = 1; p < m; p++)   // by angle
                    for (int q = p; q > 0 && ang[q] < ang[q - 1]; q--) {
                        float ta = ang[q];
                        ang[q] = ang[q - 1], ang[q - 1] = ta;
                        int ti = idx[q];
                        idx[q] = idx[q - 1], idx[q - 1] = ti;
                    }
                for (int s = 1; s + 1 < m; s += 6) {   // corner 0 and up to 7 more in each piece
                    float poly[8][3];
                    int c = 0;
                    memcpy(poly[c++], v[idx[0]], sizeof poly[0]);
                    for (int p = s; p < m && c < 8; p++) memcpy(poly[c++], v[idx[p]], sizeof poly[0]);
                    stick_emit(o, (const float (*)[3])poly, c);
                }
            }
}

#define CYL_SEG 12
static void stick_cylinder(StickOut *o, const float a[3], const float b[3], float r) {
    float ax[3], u[3], w[3];
    sub3(b, a, ax);
    float len = sqrtf(dot3(ax, ax));
    if (len < 1e-6f || r <= 0) return;
    for (int i = 0; i < 3; i++) ax[i] /= len;
    float ref[3] = {fabsf(ax[0]) < 0.9f ? 1.0f : 0.0f, fabsf(ax[0]) < 0.9f ? 0.0f : 1.0f, 0};
    cross3(ax, ref, u);
    float ul = sqrtf(dot3(u, u));
    for (int i = 0; i < 3; i++) u[i] /= ul;
    cross3(ax, u, w);
    float ra[CYL_SEG][3], rb[CYL_SEG][3];
    for (int k = 0; k < CYL_SEG; k++) {
        float c = cosf(6.2831853f * (float)k / CYL_SEG) * r, s = sinf(6.2831853f * (float)k / CYL_SEG) * r;
        for (int i = 0; i < 3; i++) ra[k][i] = a[i] + u[i] * c + w[i] * s, rb[k][i] = b[i] + u[i] * c + w[i] * s;
    }
    for (int k = 0; k < CYL_SEG; k++) {
        float q[4][3];
        int k2 = (k + 1) % CYL_SEG;
        memcpy(q[0], ra[k], sizeof q[0]), memcpy(q[1], ra[k2], sizeof q[0]);
        memcpy(q[2], rb[k2], sizeof q[0]), memcpy(q[3], rb[k], sizeof q[0]);
        stick_emit(o, (const float (*)[3])q, 4);
    }
    for (int end = 0; end < 2; end++) {   // each cap in two halves of 7 corners
        float (*ring)[3] = end ? rb : ra;
        for (int h = 0; h < 2; h++) {
            float q[7][3];
            for (int k = 0; k < 7; k++) memcpy(q[k], ring[(h * 6 + k) % CYL_SEG], sizeof q[k]);
            stick_emit(o, (const float (*)[3])q, 7);
        }
    }
}

// One entry of a shape parameter (kind 0 box, 1 polytope, 2 cylinder) that has the stick preset.
static int stick_entry(StickOut *o, const Byml *sb, uint32_t sh, int kind) {
    float rot[3];
    xyz(sb, sh, "OffsetRotation", rot);
    // degrees; the order of the three angles is not checked, as every stick shape turns about one axis at most
    for (int a = 0; a < 3; a++) rot[a] *= 3.14159265f / 180.0f;
    axes_of(rot, o->axes);
    xyz(sb, sh, "OffsetTranslation", o->t);
    if (kind == 0) {
        float c[3], h[3];
        xyz(sb, sh, "Center", c);
        if (!xyz(sb, sh, "HalfExtents", h)) return 0;
        stick_box(o, c, h);
    } else if (kind == 1) {
        uint8_t vt;
        uint32_t verts;
        float pts[HULL_MAX][3];
        int nv = 0;
        if (!dict_get(sb, sh, "Vertices", &vt, &verts) || vt != 0xc0) return 0;
        for (uint32_t q = 0; nv < HULL_MAX; q++) {
            uint8_t qt;
            uint32_t qv;
            if (!array_get(sb, verts, q, &qt, &qv)) break;
            if (qt == 0xc1) xyz_node(sb, qv, pts[nv++]);
        }
        if (nv < 4) return 0;
        stick_hull(o, (const float (*)[3])pts, nv);
    } else {
        float a[3], c[3];
        uint8_t rt;
        uint32_t rv;
        if (!xyz(sb, sh, "CenterA", a) || !xyz(sb, sh, "CenterB", c) || !dict_get(sb, sh, "Radius", &rt, &rv) || rt != 0xd2)
            return 0;
        stick_cylinder(o, a, c, f32_of(rv));
    }
    return 1;
}

int dbg_actor_stick_shapes(const char *name, int (*fn)(const float (*pts)[3], int n, void *ctx), void *ctx) {
    char path[160], member[192];
    snprintf(path, sizeof path, "Pack/Actor/%s.pack.zs", name);
    size_t pn = 0;
    uint8_t *pk = dbg_romfs_unzs(path, "pack.zsdic", &pn);
    if (!pk) return -1;
    int found = 0;
    Byml b;
    uint8_t t;
    uint32_t v, list;
    const char *cs;
    snprintf(member, sizeof member, "Component/Physics/%s.engine__component__PhysicsParam.bgyml", name);
    if (pack_key(pk, pn, member, "ControllerSetPath", &b, &t, &v) && t == 0xa0 && (cs = table_str(&b, b.strs, v)) &&
        work_member(cs, member, sizeof member) && pack_key(pk, pn, member, "ShapeNamePathAry", &b, &t, &list) && t == 0xc0) {
        StickOut o = {.fn = fn, .ctx = ctx};
        for (uint32_t i = 0; !o.stop; i++) {
            uint8_t et, pt;
            uint32_t entry, pv, root;
            const char *sp;
            char sm[192];
            size_t at, size;
            Byml sb;
            if (!array_get(&b, list, i, &et, &entry)) break;
            if (et != 0xc1 || !dict_get(&b, entry, "FilePath", &pt, &pv) || pt != 0xa0 || !(sp = table_str(&b, b.strs, pv)) ||
                !work_member(sp, sm, sizeof sm) || !sarc_member(pk, pn, sm, &at, &size) || !byml_open(pk + at, size, &sb, &root))
                continue;
            static const char *const kinds[] = {"Box", "Polytope", "Cylinder"};
            for (int kind = 0; kind < 3 && !o.stop; kind++) {
                uint8_t kt;
                uint32_t arr;
                if (!dict_get(&sb, root, kinds[kind], &kt, &arr) || kt != 0xc0) continue;
                for (uint32_t s = 0; !o.stop; s++) {
                    uint8_t st;
                    uint32_t sh;
                    if (!array_get(&sb, arr, s, &st, &sh)) break;
                    if (st == 0xc1 && sticks(&sb, sh)) found += stick_entry(&o, &sb, sh, kind);
                }
            }
        }
    }
    free(pk);
    return found;
}

// ---- the flags a save file keeps (dbg_saved_flag_keys)
// GameData/GameDataList.Product.<n>.byml.zs (zstd with zs.zsdic): a big-endian BYML ("BY", version 7) whose root's Data
// has a list per value type (Bool, Int, Float, Enum, String64, Vector3, ... and arrays, structs and the 64-bit keyed
// bools) of dictionaries {Hash, DefaultValue, ResetTypeValue, SaveFileIndex}. Hash is the flag's key (dbg_flag_key of its
// name); a SaveFileIndex of -1 marks a flag kept only while the game runs (the sequence and event state among them), any
// other the save file part it goes to. The stage files' reader above is little-endian only, so this one reads either.
typedef struct {
    const uint8_t *p;
    size_t n;
    int big;
    uint32_t keys;
} Gdl;

static int gdl_u32(const Gdl *g, size_t o, uint32_t *v) {
    if (o > g->n || g->n - o < 4) return 0;
    const uint8_t *q = g->p + o;
    *v = g->big ? (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]
                : (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    return 1;
}

// A node's head: its type (the first byte) and count (the other three).
static int gdl_head(const Gdl *g, uint32_t node, uint8_t *type, uint32_t *count) {
    if (node >= g->n || g->n - node < 4) return 0;
    const uint8_t *q = g->p + node;
    *type = q[0];
    *count = g->big ? (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3] : (uint32_t)q[3] << 16 | (uint32_t)q[2] << 8 | q[1];
    return 1;
}

static const char *gdl_key(const Gdl *g, uint32_t i) {
    uint8_t t;
    uint32_t cnt, off;
    if (!gdl_head(g, g->keys, &t, &cnt) || t != 0xc2 || i >= cnt || !gdl_u32(g, (size_t)g->keys + 4 + 4 * (size_t)i, &off))
        return NULL;
    size_t at = (size_t)g->keys + off;
    return at < g->n && memchr(g->p + at, 0, g->n - at) ? (const char *)g->p + at : NULL;
}

static int gdl_get(const Gdl *g, uint32_t node, const char *key, uint8_t *type, uint32_t *value) {
    uint8_t t;
    uint32_t cnt;
    if (!gdl_head(g, node, &t, &cnt) || t != 0xc1) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        size_t at = (size_t)node + 4 + 8 * (size_t)i;
        if (at + 8 > g->n) return 0;
        const uint8_t *q = g->p + at;
        uint32_t k = g->big ? (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2] : (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
        const char *name = gdl_key(g, k);
        if (name && !strcmp(name, key)) {
            *type = q[3];
            return gdl_u32(g, at + 4, value);
        }
    }
    return 0;
}

uint32_t *dbg_saved_flag_keys(int *n_out) {
    static const char *const lists[] = {"Bool", "Int", "UInt", "Float", "Enum", "String16", "String32", "String64", "Vector3"};
    *n_out = 0;
    const char *path = NULL;
    for (uint64_t i = 0, c = romfs_file_count(); i < c && !path; i++) {
        uint64_t off, sz;
        const char *p = romfs_entry(i, &off, &sz);
        size_t lp = p ? strlen(p) : 0;
        if (p && !strncmp(p, "GameData/GameDataList.", 22) && lp > 8 && !strcmp(p + lp - 8, ".byml.zs")) path = p;
    }
    size_t n = 0;
    uint8_t *raw = dbg_romfs_unzs(path, "zs.zsdic", &n);
    Gdl g = {raw, n, raw && n >= 16 && !memcmp(raw, "BY", 2), 0};
    uint32_t root, data, *keys = NULL;
    uint8_t t;
    int count = 0, cap = 0;
    if (raw && n >= 16 && (!memcmp(raw, "BY", 2) || !memcmp(raw, "YB", 2)) && gdl_u32(&g, 4, &g.keys) && gdl_u32(&g, 12, &root) &&
        gdl_get(&g, root, "Data", &t, &data) && t == 0xc1) {
        for (size_t l = 0; l < sizeof lists / sizeof *lists; l++) {
            uint32_t arr, cnt;
            uint8_t at;
            if (!gdl_get(&g, data, lists[l], &t, &arr) || t != 0xc0 || !gdl_head(&g, arr, &at, &cnt)) continue;
            size_t values = (size_t)arr + 4 + ((cnt + 3u) & ~3u);
            for (uint32_t i = 0; i < cnt; i++) {
                uint8_t ht, st;
                uint32_t e, hash, save;
                if (g.p[arr + 4 + i] != 0xc1 || !gdl_u32(&g, values + 4 * (size_t)i, &e)) continue;
                if (!gdl_get(&g, e, "Hash", &ht, &hash) || !gdl_get(&g, e, "SaveFileIndex", &st, &save) || (int32_t)save < 0)
                    continue;
                if (count == cap) {
                    uint32_t *v = realloc(keys, (size_t)(cap = cap ? cap * 2 : 4096) * sizeof *v);
                    if (!v) break;
                    keys = v;
                }
                keys[count++] = hash;
            }
        }
    }
    free(raw);
    *n_out = count;
    return keys;
}
