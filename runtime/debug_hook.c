// Hookshot targets and invisible walls for the debug menu's volume overlay (Debug tab, View, "Show hookshot targets" and
// "Show invisible walls"). Hookshot: the surfaces the hookshot sticks to, read from the current area's collision files,
// and the target objects placed in its stage files, drawn from their own collision shapes. Walls: the collision
// polygons of material AirWall (id 27) in the same files, and the <Model>__Airwall* shapes of the models placed in the
// area, each only where its collision filter stops the player (stops_player); the placed AirWall actors themselves are
// boxes, debug_view.c. A helper thread reads them once per area change
// or change of what is wanted; the menu asks for the ones near the player every frame (dbg_hook_polys).
//
// Surfaces. Phive/StaticCompoundBody/<folder>/<banc>[_X<i>_Z<j>].Nin_Ounce_NVN2.bphsc.zs (zstd with the zs.zsdic
// dictionary) hold an area's static collision, in tiles of 250 m. A tile starts "Phive\0" 02 02, then from +0x0c a table
// of u32 offsets that runs until one equals the file size: sections 0 to 7 (4: 20-byte records, one per shape tag, each
// naming a run of the material table: its first entry, u16 at +0x0c, and how many, u32 at +0x10; 5: the material table,
// 16-byte entries {u32 material, u32 sub-material, u64 flags}), then one Havok tag file per compound shape (an empty slot repeats the next offset), then a 0x30-byte block
// whose float3 at +0x18 places the tile's terrain, then the terrain: an embedded "Phive\0" 01 00 shape file (u32[8] at
// +0x0c: tag file, material table, filter table, end, then their sizes), a mesh whose shape tags index its own material
// table. A compound instance's shape tag picks its record; a mesh inside picks among the record's materials with its own
// tag (0xffff: none, so the first). Checked against the shape files the tiles' meshes are copies of: matched by geometry,
// their own material tables agree with this for every Gerudo object (the ladder's record 266 starts at material 110 and
// has its five).
// Material flag bit 44 is StickHookshot and bit 50 HookshotEffective: the game's flag names run from LadderUp at bit 0, which
// the data bears out on the ladders (bits 0, 1, 3: LadderUp, Ladder, LadderSide), the Lens of Truth glass (47 and 63, the
// two GlassTruthPassThrough flags), the climbable cliffs and nets (40, Climb) and the water walls (59, WallWater).
//
// Target objects. The hookshot's own targets (Obj_HookShotTarget_*, Obj_*_HookShotTarget_*, Land_*_HookTarget_*, the
// HookShotFrame / HookShotBase / HookShotDecoration pieces) are found by name among the area's placements
// (debug_place.c). Of each, only the parts whose material preset is Wall_StickHookshot are drawn, from the shape
// parameters of its actor pack (debug_place.c dbg_actor_stick_shapes), at the placement's position, rotation and scale:
// the plate of a target, not its rim (Obj_HookShotTarget_A_01__Physical_Edge, a mesh file without the preset), nor the
// stands, frames and decorations, which have no stick part at all and are left out. Most placed targets' plates are in
// the tile's collision as well (the static compound copies them with their flags), so they show among the surfaces.
//
// Havok tag files (SDK 20250200): chunks of {u32 big-endian size | flags << 30, four-character tag}. TAG0 holds SDKV,
// DATA, TYPE (TST1: the type names; TNA1: varint count, then per type {name index, template count, that many {name,
// value}}) and INDX > ITEM (12-byte {u32 type | flags << 24, offset in DATA, count}; flags 0x10: one object). Varints
// are big-endian: 0xxxxxxx, 10xxxxxx +1 byte, 110xxxxx +2, 11100xxx +3. Relative offsets count from the field itself.
// The layouts used, from the files' own type bodies for this SDK:
//   hknpCompoundShape: instances {i64 offset, i32 count} at +0x38, each 0x40 bytes: quaternion xyzw +0x00, translation
//     +0x10, scale +0x20, shape tag u16 +0x2c, the shape (i64 relative pointer) +0x30.
//   hknpMeshShape: bitScale16Inv float3 +0x50; shape tag table {i32 offset, i32 count} +0x60 of {u32 first primitive key,
//     u16 tag, u16}; geometry sections (the same) +0x88, 0x40 bytes each: primitives +0x08 (four u8 vertex indices, a
//     triangle when the last two match), vertex buffer +0x10 (three u16 a vertex), section offset i32[3] +0x20. A vertex is
//     (section offset + value) * bitScale16Inv; a primitive's key is section << 9 | primitive << 1.
//   hknpBoxShape, hknpConvexShape, hknpConvexPolytopeShape: the hull at +0x38: vertices float3 +0x00, faces {u16 first
//     index, u8 count, u8} +0x10, indices u8 +0x18, each {i32 offset, i32 count}.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

#define HOOK_FLAGS ((1ull << 44) | (1ull << 50))   // StickHookshot, HookshotEffective
#define MAT_AIRWALL 27                              // the material id of Material_AirWall
#define POLY_MAX 200000
#define LABEL_MAX 256
#define POLL_MS 500

typedef struct {
    const uint8_t *p;
    size_t n;
} Buf;

static int has(const Buf *b, size_t off, size_t len) { return off <= b->n && len <= b->n - off; }
static uint16_t u16(const Buf *b, size_t o) { uint16_t v = 0; if (has(b, o, 2)) memcpy(&v, b->p + o, 2); return v; }
static uint32_t u32(const Buf *b, size_t o) { uint32_t v = 0; if (has(b, o, 4)) memcpy(&v, b->p + o, 4); return v; }
static int32_t i32(const Buf *b, size_t o) { return (int32_t)u32(b, o); }
static int64_t i64(const Buf *b, size_t o) { int64_t v = 0; if (has(b, o, 8)) memcpy(&v, b->p + o, 8); return v; }
static uint64_t u64(const Buf *b, size_t o) { return (uint64_t)i64(b, o); }
static float f32(const Buf *b, size_t o) { float v = 0; if (has(b, o, 4)) memcpy(&v, b->p + o, 4); return v; }

// A relative array {i32 offset, i32 count} at o: its start, or 0 when it does not lie in the file with elem-byte items.
static size_t view(const Buf *b, size_t o, size_t elem, int *count) {
    int32_t off = i32(b, o), n = i32(b, o + 4);
    int64_t at = (int64_t)o + off;
    *count = 0;
    if (!has(b, o, 8) || n <= 0 || at < 0 || !has(b, (size_t)at, (size_t)n * elem)) return 0;
    *count = n;
    return (size_t)at;
}

// ---- tag files
typedef struct {
    size_t at;
    uint32_t type;
} Obj;
typedef struct {
    size_t data;
    const char **names;   // type names by index (0: none)
    int ntypes;
    Obj *objs;
    int nobjs;
} Tag;

static uint32_t chunk_size(const Buf *b, size_t o) {
    if (!has(b, o, 8)) return 0;
    const uint8_t *q = b->p + o;
    return ((uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]) & 0x3fffffffu;
}

// The chunk with this tag directly inside [o, end), descending into TYPE and INDX.
static int chunk_find(const Buf *b, size_t o, size_t end, const char *tag, size_t *at, size_t *stop) {
    while (o + 8 <= end) {
        uint32_t size = chunk_size(b, o);
        if (size < 8 || o + size > end) return 0;
        if (!memcmp(b->p + o + 4, tag, 4)) {
            *at = o + 8, *stop = o + size;
            return 1;
        }
        if ((!memcmp(b->p + o + 4, "TYPE", 4) || !memcmp(b->p + o + 4, "INDX", 4)) &&
            chunk_find(b, o + 8, o + size, tag, at, stop))
            return 1;
        o += size;
    }
    return 0;
}

static int varint(const Buf *b, size_t *o, size_t end, uint32_t *v) {
    if (*o >= end) return 0;
    uint8_t x = b->p[*o];
    int len = x < 0x80 ? 1 : x < 0xc0 ? 2 : x < 0xe0 ? 3 : x < 0xe8 ? 4 : 0;
    if (!len || *o + len > end) return 0;
    uint32_t r = len == 1 ? x : len == 2 ? x & 0x3fu : len == 3 ? x & 0x1fu : x & 7u;
    for (int i = 1; i < len; i++) r = r << 8 | b->p[*o + i];
    *o += len, *v = r;
    return 1;
}

static int obj_order(const void *a, const void *b) {
    size_t x = ((const Obj *)a)->at, y = ((const Obj *)b)->at;
    return x < y ? -1 : x > y;
}

static void tag_free(Tag *t) {
    free(t->names);
    free(t->objs);
    memset(t, 0, sizeof *t);
}

// The tag file whose TAG0 chunk starts at o.
static int tag_open(const Buf *b, size_t o, Tag *t) {
    memset(t, 0, sizeof *t);
    uint32_t size = chunk_size(b, o);
    if (size < 8 || !has(b, o, size) || memcmp(b->p + o + 4, "TAG0", 4)) return 0;
    size_t end = o + size, a, e, sa, se, na, ne, ia, ie;
    if (!chunk_find(b, o + 8, end, "SDKV", &a, &e) || e - a < 8 || memcmp(b->p + a, "20250200", 8)) return 0;
    if (!chunk_find(b, o + 8, end, "DATA", &a, &e) || !chunk_find(b, o + 8, end, "TST1", &sa, &se) ||
        !chunk_find(b, o + 8, end, "TNA1", &na, &ne) || !chunk_find(b, o + 8, end, "ITEM", &ia, &ie))
        return 0;
    t->data = a;
    // the type name strings, by index
    int ns = 0;
    for (size_t k = sa; k < se; k++) ns += b->p[k] == 0;
    const char **str = calloc((size_t)ns + 1, sizeof *str);
    if (!str) return 0;
    for (size_t k = sa, s0 = sa, i = 0; k < se && (int)i < ns; k++)
        if (!b->p[k]) str[i++] = (const char *)b->p + s0, s0 = k + 1;
    uint32_t count, v;
    size_t q = na;
    int ok = varint(b, &q, ne, &count) && count < 100000;
    t->names = ok ? calloc(count + 1, sizeof *t->names) : NULL;
    t->ntypes = ok ? (int)count : 0;
    for (uint32_t i = 1; ok && t->names && i < count; i++) {
        uint32_t name, tpl;
        ok = varint(b, &q, ne, &name) && varint(b, &q, ne, &tpl) && (int)name < ns && tpl < 64;
        if (ok) t->names[i] = str[name];
        for (uint32_t k = 0; ok && k < tpl; k++) ok = varint(b, &q, ne, &v) && varint(b, &q, ne, &v);
    }
    free(str);
    if (!ok || !t->names) {
        tag_free(t);
        return 0;
    }
    int ni = (int)((ie - ia) / 12);
    t->objs = calloc((size_t)ni + 1, sizeof *t->objs);
    if (!t->objs) {
        tag_free(t);
        return 0;
    }
    for (int i = 0; i < ni; i++) {
        uint32_t f = u32(b, ia + 12 * (size_t)i), off = u32(b, ia + 12 * (size_t)i + 4);
        if ((f >> 24) & 0x10 && (f & 0xffffff) < count) t->objs[t->nobjs++] = (Obj){t->data + off, f & 0xffffff};
    }
    qsort(t->objs, (size_t)t->nobjs, sizeof *t->objs, obj_order);
    return 1;
}

// The type name of the object at absolute offset at, "" when no object starts there.
static const char *tag_type(const Tag *t, size_t at) {
    int lo = 0, hi = t->nobjs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t->objs[mid].at == at) return t->names[t->objs[mid].type] ? t->names[t->objs[mid].type] : "";
        if (t->objs[mid].at < at) lo = mid + 1;
        else hi = mid - 1;
    }
    return "";
}

// ---- what is kept: a tile's records and materials, a shape file's own material table, or everything
typedef struct {
    const Buf *b;
    size_t rec, mat;      // absolute starts; 0 for none
    int nrec, nmat;
    int direct;           // shape tags index the material table directly (a terrain mesh)
    int all;              // keep every polygon (a target object's), or every one the player collides with (an air wall shape)
    int walls;            // keep material AirWall, else the hookshot flags
    size_t fil;           // absolute start of the collision filter table, one entry per material; 0 for none
} Keep;

// Collision filter entries: 16 bytes for each material table entry, in a table of their own beside it (a tile's section
// 6, a shape file's third table): u32 the main layers, u32 0, then u64 the sub layers the surface collides with, as bits
// of the game's sub layer list (Unspecified, Animal, Enemy, NPC, RollingNPC, ClimbingPlayer, ...). Two kinds of air wall
// never stop the player:
//   - walls for something else: the areas' outer boundaries collide with bits 0 and 5 (and the player's ragdoll), the
//     walls only a climbing player meets with bit 5 alone, while the walls for enemies, NPCs, animals, the horse, bombs
//     and the like have neither;
//   - sensor volumes: the boxes that mark indoor space for the camera and the lighting (material flags IndoorCamera,
//     IndoorWithWall, IndoorWithRoof), and the camera and event volumes, carry the air wall material too, but all of them
//     have the sub layer mask 0x77bbffff with nothing in its upper half. The player walks through them: one fills the
//     hall of the Temple of Time (Market_Child, material 532 of its tile).
#define SUB_PLAYER ((1ull << 0) | (1ull << 5))   // Unspecified, ClimbingPlayer
#define SUB_SENSOR 0x77bbffffull

static int stops_player(const Keep *k, int m) {
    if (!k->fil) return 1;
    uint64_t sub = u64(k->b, k->fil + 16 * (size_t)m + 8);
    return sub != SUB_SENSOR && (sub & SUB_PLAYER) != 0;
}

// The material table entry of a shape tag, -1 for none.
static int material_of(const Keep *k, uint32_t base, uint32_t sub) {
    if (sub == 0xffff) sub = 0;
    uint32_t m;
    if (k->direct) m = base + sub;
    else {   // the record's first material, plus the mesh's own tag among its count of them
        if ((int)base >= k->nrec) return -1;
        size_t r = k->rec + 20 * (size_t)base;
        uint32_t count = u32(k->b, r + 0x10);
        if (sub >= (count ? count : 1)) return -1;
        m = u16(k->b, r + 0x0c) + sub;
    }
    return (int)m < k->nmat ? (int)m : -1;
}

static int kept(const Keep *k, uint32_t base, uint32_t sub) {
    if (k->all && !k->walls) return 1;
    int m = material_of(k, base, sub);
    if (m < 0) return 0;
    if (k->walls && !stops_player(k, m)) return 0;
    if (k->all) return 1;
    size_t e = k->mat + 16 * (size_t)m;
    return k->walls ? u32(k->b, e) == MAT_AIRWALL : (u64(k->b, e + 8) & HOOK_FLAGS) != 0;
}

// ---- output: convex polygons in world space
typedef struct {
    DbgHookPoly p;
    float c[3];   // the middle, for the range test
    float r;      // how far its corners reach from the middle: a wall far across still counts when it comes near
} Poly;
typedef struct {
    Poly *v;
    int n, cap, target, wall;
    int kept[2];   // polygons per kind (hookshot, wall): each has its own POLY_MAX, so many surfaces leave walls room
} Out;

typedef struct {
    float m[9];   // columns: the images of x, y and z
    float t[3];
} Xf;

static void xf_apply(const Xf *x, const float v[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = x->t[i] + x->m[i] * v[0] + x->m[3 + i] * v[1] + x->m[6 + i] * v[2];
}

// a then b: the transform that applies b first
static Xf xf_mul(const Xf *a, const Xf *b) {
    Xf r;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) r.m[c * 3 + i] = a->m[i] * b->m[c * 3] + a->m[3 + i] * b->m[c * 3 + 1] + a->m[6 + i] * b->m[c * 3 + 2];
    xf_apply(a, b->t, r.t);
    return r;
}

static Xf xf_instance(const float q[4], const float t[3], const float s[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                  2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                  2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    Xf o;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) o.m[c * 3 + i] = r[c * 3 + i] * s[c];
    memcpy(o.t, t, sizeof o.t);
    return o;
}

static void emit(Out *o, const Xf *x, const float (*pts)[3], int n) {
    if (n < 3 || o->kept[o->wall != 0] >= POLY_MAX) return;
    if (o->n == o->cap) {
        int cap = o->cap ? o->cap * 2 : 1024;
        Poly *v = realloc(o->v, (size_t)cap * sizeof *v);
        if (!v) return;
        o->v = v, o->cap = cap;
    }
    Poly *p = &o->v[o->n];
    float c[3] = {0, 0, 0};
    p->p.n = n < 8 ? n : 8;
    p->p.target = o->target;
    p->p.wall = o->wall;
    for (int i = 0; i < p->p.n; i++) {
        xf_apply(x, pts[i], p->p.p[i]);
        if (!isfinite(p->p.p[i][0]) || !isfinite(p->p.p[i][1]) || !isfinite(p->p.p[i][2])) return;
        for (int k = 0; k < 3; k++) c[k] += p->p.p[i][k] / (float)p->p.n;
    }
    memcpy(p->c, c, sizeof c);
    p->r = 0;
    for (int i = 0; i < p->p.n; i++) {
        float d[3] = {p->p.p[i][0] - c[0], p->p.p[i][1] - c[1], p->p.p[i][2] - c[2]};
        p->r = fmaxf(p->r, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    o->kept[o->wall != 0]++;
    o->n++;
}

static uint32_t mesh_tag(const Buf *b, size_t table, int n, uint32_t key) {
    int lo = 0, hi = n - 1;
    uint32_t tag = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (u32(b, table + 8 * (size_t)mid) <= key) tag = u16(b, table + 8 * (size_t)mid + 4), lo = mid + 1;
        else hi = mid - 1;
    }
    return tag;
}

static void mesh(const Buf *b, size_t m, const Xf *x, uint32_t base, const Keep *k, Out *o) {
    int nt, ns;
    size_t table = view(b, m + 0x60, 8, &nt), secs = view(b, m + 0x88, 0x40, &ns);
    if (!secs) return;
    if (!k->all) {   // nothing to draw unless one of its tags is kept
        int any = !nt && kept(k, base, 0);
        for (int i = 0; i < nt && !any; i++) any = kept(k, base, u16(b, table + 8 * (size_t)i + 4));
        if (!any) return;
    }
    float inv[3] = {f32(b, m + 0x50), f32(b, m + 0x54), f32(b, m + 0x58)};
    for (int s = 0; s < ns; s++) {
        size_t g = secs + 0x40 * (size_t)s;
        int np, nv;
        size_t prims = view(b, g + 0x08, 4, &np), verts = view(b, g + 0x10, 1, &nv);
        if (!prims || !verts) continue;
        nv /= 6;
        int32_t so[3] = {i32(b, g + 0x20), i32(b, g + 0x24), i32(b, g + 0x28)};
        for (int i = 0; i < np && i < 256; i++) {
            uint32_t tag = nt ? mesh_tag(b, table, nt, (uint32_t)s << 9 | (uint32_t)i << 1) : 0;
            if (!kept(k, base, tag)) continue;
            const uint8_t *id = b->p + prims + 4 * (size_t)i;
            float v[4][3];
            int bad = 0;
            for (int c = 0; c < 4; c++) {
                if (id[c] >= nv) {
                    bad = 1;
                    break;
                }
                for (int a = 0; a < 3; a++) v[c][a] = (float)(so[a] + (int32_t)u16(b, verts + 6 * (size_t)id[c] + 2 * a)) * inv[a];
            }
            if (bad) continue;
            if (id[2] == id[3]) emit(o, x, v, 3);
            else {
                float t2[3][3];
                memcpy(t2[0], v[0], 12), memcpy(t2[1], v[1], 12), memcpy(t2[2], v[2], 12);
                emit(o, x, t2, 3);
                memcpy(t2[1], v[2], 12), memcpy(t2[2], v[3], 12);
                emit(o, x, t2, 3);
            }
        }
    }
}

static void hull(const Buf *b, size_t h, const Xf *x, Out *o) {
    int nv, nf, ni;
    size_t verts = view(b, h, 12, &nv), faces = view(b, h + 0x10, 4, &nf), idx = view(b, h + 0x18, 1, &ni);
    if (!verts || !faces || !idx) return;
    for (int f = 0; f < nf; f++) {
        int first = u16(b, faces + 4 * (size_t)f), cnt = b->p[faces + 4 * (size_t)f + 2];
        if (cnt < 3 || first + cnt > ni) continue;
        float pts[64][3];
        int n = 0;
        for (int j = 0; j < cnt && n < 64; j++) {
            int vi = b->p[idx + first + j];
            if (vi >= nv) break;
            for (int a = 0; a < 3; a++) pts[n][a] = f32(b, verts + 12 * (size_t)vi + 4 * a);
            n++;
        }
        // a face of more than 8 corners as a fan of polygons of at most 8
        for (int s = 1; s + 1 < n; s += 6) {
            float part[8][3];
            int m = 0;
            memcpy(part[m++], pts[0], 12);
            for (int j = s; j < n && m < 8; j++) memcpy(part[m++], pts[j], 12);
            emit(o, x, part, m);
        }
    }
}

static void shape(const Buf *b, const Tag *t, size_t sp, const Xf *x, uint32_t base, int depth, const Keep *k, Out *o) {
    const char *ty = tag_type(t, sp);
    if (!strcmp(ty, "hknpCompoundShape")) {
        if (depth > 4) return;
        int64_t off = i64(b, sp + 0x38);
        int n = i32(b, sp + 0x40);
        int64_t at = (int64_t)sp + 0x38 + off;
        if (n <= 0 || at < 0 || !has(b, (size_t)at, (size_t)n * 0x40)) return;
        for (int i = 0; i < n; i++) {
            size_t e = (size_t)at + 0x40 * (size_t)i;
            float q[4], tr[3], sc[3];
            for (int a = 0; a < 4; a++) q[a] = f32(b, e + 4 * a);
            for (int a = 0; a < 3; a++) tr[a] = f32(b, e + 0x10 + 4 * a), sc[a] = f32(b, e + 0x20 + 4 * a);
            uint32_t tag = u16(b, e + 0x2c);
            int64_t rp = i64(b, e + 0x30);
            if (!rp || (int64_t)e + 0x30 + rp < 0) continue;   // a free slot
            if (depth == 0 && tag == 0xffff && !k->all) continue;
            Xf ix = xf_instance(q, tr, sc), cx = xf_mul(x, &ix);
            shape(b, t, (size_t)((int64_t)e + 0x30 + rp), &cx, depth == 0 ? tag : base, depth + 1, k, o);
        }
    } else if (!strcmp(ty, "hknpMeshShape")) {
        mesh(b, sp, x, base, k, o);
    } else if (!strcmp(ty, "hknpBoxShape") || !strcmp(ty, "hknpConvexShape") || !strcmp(ty, "hknpConvexPolytopeShape")) {
        if (kept(k, base, 0)) hull(b, sp + 0x38, x, o);
    }
}

static const Xf kIdentity = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};

// The root object (item 1) of the tag file at o.
static void tag_root(const Buf *b, size_t o, const Xf *x, const Keep *k, Out *out) {
    Tag t;
    if (!tag_open(b, o, &t)) return;
    if (t.nobjs) shape(b, &t, t.objs[0].at, x, 0, 0, k, out);
    tag_free(&t);
}

// An embedded "Phive\0" 01 00 shape file at ph: its root shape with its own material table.
static void shape_file(const Buf *b, size_t ph, const Xf *x, int all, int walls, Out *out) {
    if (!has(b, ph, 0x2c) || memcmp(b->p + ph, "Phive\0\x01\0", 8)) return;
    uint32_t tag = u32(b, ph + 0x0c), mat = u32(b, ph + 0x10), fil = u32(b, ph + 0x14), mat_n = u32(b, ph + 0x20),
             fil_n = u32(b, ph + 0x24);
    Keep k = {b, 0, ph + mat, 0, (int)(mat_n / 16), 1, all, walls, fil_n == mat_n && has(b, ph + fil, fil_n) ? ph + fil : 0};
    if (!has(b, ph + mat, mat_n)) k.nmat = 0;
    tag_root(b, ph + tag, x, &k, out);
}

// One collision tile: its compounds and its terrain, the polygons of the hookshot or the air walls. shift is the tile's
// place in the area for terrain blocks that carry none (see tile_shift).
static void tile(const Buf *b, const float shift[2], int walls, Out *out) {
    if (!has(b, 0, 0x10) || memcmp(b->p, "Phive\0\x02\x02", 8)) return;
    uint32_t off[64];
    int n = 0;
    while (n < 64) {
        off[n] = u32(b, 0x0c + 4 * (size_t)n);
        if (!has(b, 0x0c + 4 * (size_t)n, 4) || off[n++] >= b->n) break;
    }
    if (n < 12 || off[n - 1] != b->n) return;
    if (off[4] > off[5] || off[5] > off[6] || off[6] > off[7]) return;
    Keep k = {b, off[4], off[5], (int)((off[5] - off[4]) / 20), (int)((off[6] - off[5]) / 16), 0, 0, walls,
              off[7] - off[6] == off[6] - off[5] ? off[6] : 0};
    out->wall = walls;   // the compounds' air walls as well as the terrain's
    for (int i = 8; i < n - 3; i++)
        if (off[i] < off[i + 1]) tag_root(b, off[i], &kIdentity, &k, out);
    Xf terrain = kIdentity;
    int placed = 0;
    for (int a = 0; a < 3; a++) {
        terrain.t[a] = f32(b, off[n - 3] + 0x18 + 4 * a);
        placed |= fabsf(terrain.t[a]) >= 1e-6f;
    }
    if (!placed) terrain.t[0] = shift[0], terrain.t[2] = shift[1];
    shape_file(b, off[n - 2], &terrain, 0, walls, out);
    out->wall = 0;
}

// Where a tile's terrain goes when its placement block holds none. Hyrule Field's tiles (HyruleField_Adult_X<i>_Z<j>)
// have only tiny denormals there, and their terrain meshes run from 0 to 250 m in the tile's own frame; tile index 8 is
// the world origin (checked against the placed objects of each tile, which are in world coordinates, and against the
// ground under the field's placed air walls). Tiles of other areas carry a real offset and keep it.
static void tile_shift(const char *rest, float shift[2]) {
    int i, j;
    shift[0] = shift[1] = 0;
    if (sscanf(rest, "_X%d_Z%d", &i, &j) == 2) shift[0] = (float)(i - 8) * 250.f, shift[1] = (float)(j - 8) * 250.f;
}

// ---- the area's data, swapped in whole by the helper thread
static struct {
    PlatMutex lock;
    Poly *polys;
    int n;
    DbgHookLabel labels[LABEL_MAX];
    int nlabels;
    char status[160];
    volatile int want, running;   // want: DBG_HOOK_ bits
} H = {PLAT_MUTEX_INIT};

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int ci_has(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 0x20) == (what[i] | 0x20)) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int is_target(const char *name) {
    return (ci_has(name, "hookshot") || ci_has(name, "hooktarget")) && strncmp(name, "Item_", 5) && !ci_has(name, "airwall");
}

typedef struct {
    Out *out;
    DbgHookLabel *labels;
    int nlabels, shapes;
    int surfaces;   // out->v[0..surfaces): the tiles' polygons, read before the targets
} Targets;

// The polygons of one target object's stick parts, in world space, before they are kept.
#define STICK_POLYS 64
typedef struct {
    const Xf *x;
    float p[STICK_POLYS][8][3];
    int nc[STICK_POLYS], n;
} Sticks;

static int stick_poly(const float (*pts)[3], int n, void *ctx) {
    Sticks *s = ctx;
    if (s->n == STICK_POLYS) return 0;
    for (int i = 0; i < n; i++) xf_apply(s->x, pts[i], s->p[s->n][i]);
    s->nc[s->n++] = n;
    return 1;
}

// Only the parts the hookshot sticks to (debug_place.c dbg_actor_stick_shapes): a target's rim, stand, frame or
// decoration is not drawn, and an object with no such part is not listed. A static target's plate is usually in the
// tile's collision already, with the stick flags, and is drawn among the surfaces; such a plate is not drawn twice.
static int target_one(const DbgPlacement *pl, void *ctx) {
    Targets *tg = ctx;
    if (!is_target(pl->name)) return 1;
    Xf x;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) x.m[c * 3 + i] = pl->axes[c * 3 + i] * pl->scale[c];
    memcpy(x.t, pl->pos, sizeof x.t);
    static Sticks s;
    s.x = &x, s.n = 0;
    if (dbg_actor_stick_shapes(pl->name, stick_poly, &s) <= 0 || !s.n) return 1;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < s.n; i++)
        for (int k = 0; k < s.nc[i]; k++)
            for (int a = 0; a < 3; a++) lo[a] = fminf(lo[a], s.p[i][k][a]), hi[a] = fmaxf(hi[a], s.p[i][k][a]);
    int in_tile = 0;
    for (int i = 0; i < tg->surfaces && !in_tile; i++) {
        const Poly *q = &tg->out->v[i];
        if (q->p.wall) continue;
        in_tile = 1;
        for (int a = 0; a < 3; a++) in_tile &= q->c[a] >= lo[a] - 0.05f && q->c[a] <= hi[a] + 0.05f;
    }
    if (!in_tile) {
        Xf id = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
        tg->out->target = 1;
        for (int i = 0; i < s.n; i++) emit(tg->out, &id, (const float (*)[3])s.p[i], s.nc[i]);
        tg->out->target = 0;
        tg->shapes++;
    }
    if (tg->nlabels < LABEL_MAX) {
        DbgHookLabel *l = &tg->labels[tg->nlabels++];
        for (int a = 0; a < 3; a++) l->pos[a] = (lo[a] + hi[a]) * 0.5f;
        snprintf(l->name, sizeof l->name, "%s", pl->name);
        l->shaped = 1;
    }
    return 1;
}

// ---- air wall shapes of placed models: Phive/Shape/**/<Model>__Airwall*, listed once
typedef struct {
    char model[80];
    const char *path;   // romfs path, valid while the romfs is
} WallShape;
static WallShape *g_wall_shapes;
static int g_nwall_shapes = -1;

static int by_model(const void *a, const void *b) { return strcmp(((const WallShape *)a)->model, ((const WallShape *)b)->model); }

static void wall_shapes_list(void) {
    if (g_nwall_shapes >= 0) return;
    int n = 0, cap = 0;
    for (uint64_t i = 0, c = romfs_file_count(); i < c; i++) {
        uint64_t off, sz;
        const char *p = romfs_entry(i, &off, &sz);
        if (!p || strncmp(p, "Phive/Shape/", 12)) continue;
        const char *f = base_name(p), *sep = strstr(f, "__");
        if (!sep || !ci_has(sep, "airwall") || (size_t)(sep - f) >= sizeof g_wall_shapes->model) continue;
        if (n == cap) {
            WallShape *v = realloc(g_wall_shapes, (size_t)(cap = cap ? cap * 2 : 512) * sizeof *v);
            if (!v) break;
            g_wall_shapes = v;
        }
        memcpy(g_wall_shapes[n].model, f, (size_t)(sep - f));
        g_wall_shapes[n].model[sep - f] = 0;
        g_wall_shapes[n++].path = p;
    }
    qsort(g_wall_shapes, (size_t)n, sizeof *g_wall_shapes, by_model);
    g_nwall_shapes = n;
}

typedef struct {
    Out *out;
    int models;
} Walls;

static int wall_one(const DbgPlacement *pl, void *ctx) {
    Walls *w = ctx;
    int lo = 0, hi = g_nwall_shapes - 1, at = -1;
    while (lo <= hi) {   // the first shape of this model
        int mid = (lo + hi) / 2, c = strcmp(g_wall_shapes[mid].model, pl->name);
        if (c >= 0) hi = mid - 1;
        if (!c) at = mid;
        if (c < 0) lo = mid + 1;
    }
    if (at < 0) return 1;
    Xf x;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) x.m[c * 3 + i] = pl->axes[c * 3 + i] * pl->scale[c];
    memcpy(x.t, pl->pos, sizeof x.t);
    w->out->wall = 1;
    for (int s = at; s < g_nwall_shapes && !strcmp(g_wall_shapes[s].model, pl->name); s++) {
        size_t bn = 0;
        uint8_t *raw = dbg_romfs_unzs(g_wall_shapes[s].path, "zs.zsdic", &bn);
        if (!raw) continue;
        Buf b = {raw, bn};
        shape_file(&b, 0, &x, 1, 1, w->out);
        free(raw);
    }
    w->out->wall = 0;
    w->models++;
    return 1;
}

static void load_area(const char *area, int want) {
    Out out = {0};
    size_t la = strlen(area);
    int tiles = 0, hook_polys = 0, wall_polys = 0, wall_models = 0;
    uint64_t t0 = plat_now_ns();
    for (uint64_t i = 0, n = romfs_file_count(); i < n && H.want; i++) {
        uint64_t off, sz;
        const char *p = romfs_entry(i, &off, &sz);
        if (!p || strncmp(p, "Phive/StaticCompoundBody/", 25)) continue;
        const char *f = base_name(p);
        if (strncmp(f, area, la) || (strncmp(f + la, "_X", 2) && strcmp(f + la, ".Nin_Ounce_NVN2.bphsc.zs"))) continue;
        size_t bn = 0;
        uint8_t *raw = dbg_romfs_unzs(p, "zs.zsdic", &bn);
        if (!raw) continue;
        Buf b = {raw, bn};
        float shift[2];
        tile_shift(f + la, shift);
        int before = out.n;
        if (want & DBG_HOOK_SURFACES) tile(&b, shift, 0, &out);
        hook_polys += out.n - before, before = out.n;
        if (want & DBG_HOOK_WALLS) tile(&b, shift, 1, &out);
        wall_polys += out.n - before;
        free(raw);
        tiles++;
    }
    int surfaces = hook_polys;
    static DbgHookLabel labels[LABEL_MAX];
    Targets tg = {&out, labels, 0, 0, out.n};
    if (want & DBG_HOOK_SURFACES) dbg_place_each(area, target_one, &tg);
    if (want & DBG_HOOK_WALLS) {
        wall_shapes_list();
        int before = out.n;
        Walls w = {&out, 0};
        dbg_place_each(area, wall_one, &w);
        wall_models = w.models, wall_polys += out.n - before;
    }
    plat_mutex_lock(&H.lock);
    free(H.polys);
    H.polys = out.v, H.n = out.n;
    memcpy(H.labels, labels, (size_t)tg.nlabels * sizeof *labels);
    H.nlabels = tg.nlabels;
    snprintf(H.status, sizeof H.status, "%s: %d collision tiles; hookshot %d surface polygons, %d target objects; "
             "air walls %d polygons (%d placed models)", area, tiles, surfaces, tg.nlabels, wall_polys, wall_models);
    plat_mutex_unlock(&H.lock);
    rt_log("debug hook: %s: %d collision tiles; %d hookshot surface polygons, %d target objects (%d with a shape, %d "
           "polygons); %d air wall polygons, %d placed models with an air wall shape; %.0f ms",
           area, tiles, surfaces, tg.nlabels, tg.shapes, out.n - surfaces - wall_polys, wall_polys, wall_models,
           (double)(plat_now_ns() - t0) / 1e6);
}

static void hook_thread(void *arg) {
    (void)arg;
    char loaded[64] = "";
    int had = 0;
    while (H.want) {
        char area[64];
        int want = H.want;
        if (dbg_current_area(area) && area[0] && (strcmp(area, loaded) || want != had)) {
            load_area(area, want);
            memcpy(loaded, area, sizeof loaded);
            had = want;
        }
        for (int t = 0; t < POLL_MS / 50 && H.want; t++) plat_sleep_ms(50);
    }
    plat_mutex_lock(&H.lock);
    free(H.polys);
    H.polys = NULL, H.n = 0, H.nlabels = 0, H.status[0] = 0;
    plat_mutex_unlock(&H.lock);
    __atomic_store_n(&H.running, 0, __ATOMIC_RELEASE);
}

void dbg_hook_want(int want) {
    H.want = want;
    int zero = 0;
    if (want && __atomic_compare_exchange_n(&H.running, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
        !plat_thread_spawn(hook_thread, NULL, "debug-hook", 0))
        H.running = 0;
}

static int near(const Poly *q, const float me[3], float range) {
    const float *p = q->c;
    float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
    float r = range + q->r;
    return d <= r * r;
}

int dbg_hook_polys(DbgHookPoly *out, int max, float range, int walls) {
    float me[3];
    if (!dbg_player_pos(me) || max <= 0) return 0;
    int n = 0;
    walls = walls != 0;
    plat_mutex_lock(&H.lock);
    // a polygon is in range when any part of it may be: its middle within range plus its own reach. Nearest first
    // when more are in range than fit: the radius shrinks until they do.
    for (int pass = 0; pass < 8; pass++) {
        int c = 0;
        for (int i = 0; i < H.n; i++) c += H.polys[i].p.wall == walls && near(&H.polys[i], me, range);
        if (c <= max) break;
        range *= 0.7071f;
    }
    for (int i = 0; i < H.n && n < max; i++)
        if (H.polys[i].p.wall == walls && near(&H.polys[i], me, range)) out[n++] = H.polys[i].p;
    plat_mutex_unlock(&H.lock);
    return n;
}

int dbg_hook_labels(DbgHookLabel *out, int max, float range) {
    float me[3];
    if (!dbg_player_pos(me)) return 0;
    int n = 0;
    plat_mutex_lock(&H.lock);
    for (int i = 0; i < H.nlabels && n < max; i++) {
        const float *p = H.labels[i].pos;
        float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d <= range * range) out[n++] = H.labels[i];
    }
    plat_mutex_unlock(&H.lock);
    return n;
}

void dbg_hook_status(char *out, size_t n) {
    plat_mutex_lock(&H.lock);
    snprintf(out, n, "%s", H.status[0] ? H.status : (H.want ? "reading the area's collision" : "off"));
    plat_mutex_unlock(&H.lock);
}
