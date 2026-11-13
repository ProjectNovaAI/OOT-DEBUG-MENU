// Clips (clip.h): the game's picture and sound are encoded all the time with Windows' own H.264 and AAC encoders
// (Media Foundation, the graphics card's encoder when it offers one) into a fragmented MP4 kept in memory, and the
// fragments of the last clip_seconds() are written to a file when the player asks.
//
// Frames: the present (gfx_present.c) copies the swapchain image, scaled down to at most 1920 x 1080, into one of its
// clip buffers and hands it over once that frame's fence has signalled (clip_frame); the recorder thread converts it
// to NV12 and gives the buffer back. A frame that arrives while the recorder is still busy replaces the waiting one.
// Sound: audio_mixer.c passes every block it sends to the device (clip_audio) into a ring the recorder reads, placed
// in time by when it was sent; gaps are filled with silence so the audio track never runs short of the picture.
//
// The writer is a sink writer on an in-memory byte stream (mem_*) with the fragmented MP4 container: ftyp and moov
// first, then a moof + mdat pair for each fragment. Everything before the first moof is kept; whole fragments are
// kept for clip_seconds() + KEEP_EXTRA_S. A clip is that header followed by the fragments from the last key frame at
// or before the start, with every fragment's decode time (tfdt) moved back so the clip starts at 0.
//
// Media Foundation is loaded when recording starts (mf_load), so a Windows without it (the N editions without the
// Media Feature Pack) still runs the game; recording then stays off and the menu says why.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "clip.h"
#include "debug_tools.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"

// The encoder's settings asked for through ICodecAPI (codecapi.h names them only for C++).
#define CLIP_GUID(name, ...) DEFINE_GUID(name, __VA_ARGS__)
CLIP_GUID(CLIP_GOP_SIZE, STATIC_CODECAPI_AVEncMPVGOPSize);
CLIP_GUID(CLIP_B_FRAMES, STATIC_CODECAPI_AVEncMPVDefaultBPictureCount);
static const GUID clip_no_service;   // GetServiceForStream's "the stream's own objects"

#define FPS 60
#define FRAME_NS (1000000000ull / FPS)
#define GOP 30                  // a key frame every half second: where a clip can start
#define KEEP_EXTRA_S 4          // fragments kept beyond the clip length
#define AUDIO_RATE 48000
#define AUDIO_RING (AUDIO_RATE * 2)   // stereo frames: two seconds
#define AUDIO_CHUNK 1024        // frames per audio sample written
#define AUDIO_SLACK 2400        // 50 ms: a larger gap between where the audio is and where it should be is closed
#define SAVE_WAIT_NS 4000000000ull   // a save waits up to this long for the fragment that holds its last moment
#define SECONDS_MIN 5
#define SECONDS_MAX 120

// ---- Media Foundation, loaded by name
typedef HRESULT(WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT(WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT(WINAPI *PFN_MFCreateSample)(IMFSample **);
typedef HRESULT(WINAPI *PFN_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT(WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *PFN_MFCreateAsyncResult)(IUnknown *, IMFAsyncCallback *, IUnknown *, IMFAsyncResult **);
typedef HRESULT(WINAPI *PFN_MFInvokeCallback)(IMFAsyncResult *);
typedef HRESULT(WINAPI *PFN_MFCreateSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
static struct {
    PFN_MFStartup Startup;
    PFN_MFCreateMediaType CreateMediaType;
    PFN_MFCreateSample CreateSample;
    PFN_MFCreateMemoryBuffer CreateMemoryBuffer;
    PFN_MFCreateAttributes CreateAttributes;
    PFN_MFCreateAsyncResult CreateAsyncResult;
    PFN_MFInvokeCallback InvokeCallback;
    PFN_MFCreateSinkWriterFromURL CreateSinkWriterFromURL;
} mf;
static int mf_state;   // 0 not tried, 1 loaded and started, -1 not available
static char mf_why[96];

static int mf_load(void) {
    if (mf_state) return mf_state > 0;
    mf_state = -1;
    HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
    if (!plat || !rw) {
        snprintf(mf_why, sizeof mf_why, "Windows Media Foundation is not installed (Media Feature Pack)");
        return 0;
    }
#define GET(m, f, n) (*(FARPROC *)&mf.f = GetProcAddress(m, n))
    if (!GET(plat, Startup, "MFStartup") || !GET(plat, CreateMediaType, "MFCreateMediaType") ||
        !GET(plat, CreateSample, "MFCreateSample") || !GET(plat, CreateMemoryBuffer, "MFCreateMemoryBuffer") ||
        !GET(plat, CreateAttributes, "MFCreateAttributes") || !GET(plat, CreateAsyncResult, "MFCreateAsyncResult") ||
        !GET(plat, InvokeCallback, "MFInvokeCallback") || !GET(rw, CreateSinkWriterFromURL, "MFCreateSinkWriterFromURL")) {
        snprintf(mf_why, sizeof mf_why, "Windows Media Foundation is incomplete on this system");
        return 0;
    }
#undef GET
    HRESULT hr = mf.Startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        snprintf(mf_why, sizeof mf_why, "Media Foundation did not start (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    mf_state = 1;
    return 1;
}

// ---- settings
static _Atomic int enabled = -1, seconds = 0, overlays = 0;
static const char *const view2_keys[CLIP_VIEW2_MODES] = {"off", "above", "side", "front"};
static void settings_load(void) {
    if (atomic_load(&enabled) >= 0) return;
    atomic_store(&overlays, cfg_bool("NX2NAVI_DEBUG_CLIP_OVERLAYS") != 0);
    int s = cfg_int("NX2NAVI_DEBUG_CLIP_SECONDS");
    atomic_store(&seconds, s < SECONDS_MIN ? SECONDS_MIN : s > SECONDS_MAX ? SECONDS_MAX : s);
    atomic_store(&enabled, cfg_bool("NX2NAVI_DEBUG_CLIP") != 0);
}
int clip_enabled(void) { settings_load(); return atomic_load(&enabled) == 1; }
int clip_seconds(void) { settings_load(); return atomic_load(&seconds); }
void clip_set_enabled(int on) {
    settings_load();
    atomic_store(&enabled, on != 0);
    dbg_keep("NX2NAVI_DEBUG_CLIP", on ? "1" : "0");
}
int clip_overlays(void) { settings_load(); return atomic_load(&overlays) == 1; }
void clip_set_overlays(int on) {
    settings_load();
    atomic_store(&overlays, on != 0);
    dbg_keep("NX2NAVI_DEBUG_CLIP_OVERLAYS", on ? "1" : "0");
}
// read from the setting each time (a few calls a frame): the console's cfg command changes it too
int clip_view2(void) {
    const char *v = cfg_value("NX2NAVI_DEBUG_CLIP_VIEW2");
    for (int i = 1; v && i < CLIP_VIEW2_MODES; i++)
        if (!strcmp(v, view2_keys[i])) return i;
    return 0;
}
void clip_set_view2(int mode) {
    mode = mode < 0 ? 0 : mode >= CLIP_VIEW2_MODES ? CLIP_VIEW2_MODES - 1 : mode;
    dbg_keep("NX2NAVI_DEBUG_CLIP_VIEW2", view2_keys[mode]);
}
const char *clip_view2_name(int mode) {
    static const char *const names[CLIP_VIEW2_MODES] = {"Off", "Above", "Beside", "In front"};
    return mode >= 0 && mode < CLIP_VIEW2_MODES ? names[mode] : "Off";
}
void clip_set_seconds(int s) {
    settings_load();
    s = s < SECONDS_MIN ? SECONDS_MIN : s > SECONDS_MAX ? SECONDS_MAX : s;
    atomic_store(&seconds, s);
    char v[16];
    snprintf(v, sizeof v, "%d", s);
    dbg_keep("NX2NAVI_DEBUG_CLIP_SECONDS", v);
}

// ---- the recorder's thread and what is handed to it (lk)
static PlatMutex lk;
static PlatCond wake;
static _Atomic int started;
static uint64_t last_due_ns;
static struct Pending {
    int buf, w, h, stride, kind;
    const uint8_t *px;
    uint64_t ns;
} pending = {-1};
static int save_asked;
static uint64_t save_ns;
static void recorder(void *arg);

int clip_frame_due(uint64_t now_ns) {
    if (!clip_enabled() || mf_state < 0) return 0;
    if (!atomic_load(&started) && !atomic_exchange(&started, 1)) {
        plat_mutex_init(&lk);
        plat_cond_init(&wake);
        if (!plat_thread_spawn(recorder, NULL, "clip recorder", 0)) {
            rt_log("clip: the recorder thread did not start");
            mf_state = -1;
            return 0;
        }
    }
    if (now_ns - last_due_ns < FRAME_NS - FRAME_NS / 16) return 0;   // a frame a sixtieth of a second, a little early is fine
    last_due_ns = now_ns;
    return 1;
}

void clip_frame(int buf, const uint8_t *px, int w, int h, int stride, int kind, uint64_t ns) {
    int drop = -1;
    plat_mutex_lock(&lk);
    if (pending.buf >= 0) drop = pending.buf;   // the recorder has not taken the last one: this one replaces it
    pending = (struct Pending){buf, w, h, stride, kind, px, ns};
    plat_cond_signal(&wake);
    plat_mutex_unlock(&lk);
    if (drop >= 0) vk_clip_release(drop);
}

// ---- sound: a ring of 16-bit stereo frames at the device's rate, each block placed by the time it was sent
static int16_t aring[AUDIO_RING * 2];
static _Atomic uint64_t a_wr, a_stamp_pos, a_stamp_ns;
static _Atomic int a_rate;
static uint64_t a_rd;

void clip_audio(const float *pcm, uint32_t frames, int channels, int rate) {
    if (atomic_load(&enabled) != 1 || !atomic_load(&started) || channels < 1) return;
    uint64_t w = atomic_load_explicit(&a_wr, memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        float l = pcm[(size_t)i * channels], r = channels > 1 ? pcm[(size_t)i * channels + 1] : l;
        if (channels >= 6) {   // 5.1 (L R C LFE Ls Rs): the centre and surrounds folded in
            float c = pcm[(size_t)i * channels + 2] * 0.7071f;
            l = l + c + pcm[(size_t)i * channels + 4] * 0.7071f;
            r = r + c + pcm[(size_t)i * channels + 5] * 0.7071f;
        }
        l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        uint64_t k = (w + i) % AUDIO_RING;
        aring[2 * k] = (int16_t)(l * 32767.0f);
        aring[2 * k + 1] = (int16_t)(r * 32767.0f);
    }
    atomic_store(&a_rate, rate);
    atomic_store(&a_stamp_ns, plat_now_ns());
    atomic_store(&a_stamp_pos, w + frames);
    atomic_store_explicit(&a_wr, w + frames, memory_order_release);
}

// ---- the in-memory file (fl): the header before the first moof, the fragments kept, and the bytes not yet split
typedef struct {
    uint8_t *data;      // moof then mdat
    size_t len, moof_len;
    uint64_t pos;       // where the moof was in the writer's file (for an absolute base_data_offset)
    uint64_t start[4];  // per track (tracks[] index): decode time of its first sample, in the track's time scale
    uint8_t has[4];     // the fragment holds samples of that track
    double t_key;       // decode time of its first video key frame in seconds, -1 without one
    double t_first;     // the earliest decode time of its track fragments, -1 without any
    double t_end;       // the latest decode time just after its samples
} Frag;
static PlatMutex fl;
static uint8_t *head;   // ftyp, moov
static size_t head_len, head_cap;
static int head_done;   // the first moof was seen: head is complete
static uint8_t *tail;   // from tail_base on, not yet split into boxes
static size_t tail_len, tail_cap;
static uint64_t tail_base, wpos, wlen;
static Frag *frags;
static int nfrag, capfrag;
static Frag open_frag;  // a moof waiting for its mdat
// A track of the moov: trex's default duration and flags, and the decode time its next fragment starts at (the
// writer leaves tfdt out, so a fragment's time is the sum of the durations before it).
static struct Track { uint32_t id, scale, dflt_dur, dflt_flags; int video; uint64_t next; } tracks[4];
static int ntracks;
static size_t kept_bytes;
static uint32_t last_write;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }

// The box at p (n bytes left): its size and the size of its header; 0 when it is not complete or not plausible.
static uint64_t box_at(const uint8_t *p, size_t n, size_t *hdr) {
    if (n < 8) return 0;
    uint64_t s = be32(p);
    *hdr = 8;
    if (s == 1) {
        if (n < 16) return 0;
        s = be64(p + 8), *hdr = 16;
    }
    return s >= *hdr ? s : 0;
}
// The first child box of type t in [p, p + n).
static uint8_t *child(uint8_t *p, size_t n, const char *t, size_t *len) {
    size_t hdr;
    for (size_t o = 0; o + 8 <= n;) {
        uint64_t s = box_at(p + o, n - o, &hdr);
        if (!s || o + s > n) return NULL;
        if (!memcmp(p + o + 4, t, 4)) {
            *len = (size_t)s;
            return p + o;
        }
        o += (size_t)s;
    }
    return NULL;
}

static int track_index(uint32_t id) {
    for (int i = 0; i < ntracks; i++)
        if (tracks[i].id == id) return i;
    return -1;
}

// The tracks' ids, time scales and kinds from the moov in head, then their defaults from mvex/trex.
static void read_moov(void) {
    size_t ml, l;
    uint8_t *moov = child(head, head_len, "moov", &ml), *p;
    ntracks = 0;
    if (!moov) return;
    for (size_t o = 8; o + 8 <= ml && ntracks < 4;) {
        size_t hdr;
        uint64_t s = box_at(moov + o, ml - o, &hdr);
        if (!s || o + s > ml) break;
        uint8_t *b = moov + o;
        if (!memcmp(b + 4, "trak", 4)) {
            struct Track t = {0};
            size_t tl = (size_t)s;
            if ((p = child(b + 8, tl - 8, "tkhd", &l)) && l >= 32) t.id = be32(p + (p[8] ? 28 : 20));
            uint8_t *md = child(b + 8, tl - 8, "mdia", &l);
            size_t mdl = l;
            if (md && (p = child(md + 8, mdl - 8, "mdhd", &l)) && l >= 32) t.scale = be32(p + (p[8] ? 28 : 20));
            if (md && (p = child(md + 8, mdl - 8, "hdlr", &l)) && l >= 20) t.video = !memcmp(p + 16, "vide", 4);
            if (t.id && t.scale) tracks[ntracks++] = t;
        }
        o += (size_t)s;
    }
    if ((p = child(moov + 8, ml - 8, "mvex", &l)))
        for (size_t q = 8; q + 8 <= l;) {
            size_t h2;
            uint64_t s2 = box_at(p + q, l - q, &h2);
            if (!s2 || q + s2 > l) break;
            if (!memcmp(p + q + 4, "trex", 4) && s2 >= 32) {
                int i = track_index(be32(p + q + 12));
                if (i >= 0) tracks[i].dflt_dur = be32(p + q + 20), tracks[i].dflt_flags = be32(p + q + 28);
            }
            q += (size_t)s2;
        }
}

// The optional fields of a tfhd (flags at +8): where each one is, 0 when absent.
typedef struct { size_t base, dur, size, flags; } TfhdAt;
static TfhdAt tfhd_at(const uint8_t *tfhd) {
    uint32_t f = be32(tfhd + 8) & 0xffffff;
    TfhdAt a = {0};
    size_t q = 16;
    if (f & 0x1) a.base = q, q += 8;
    if (f & 0x2) q += 4;
    if (f & 0x8) a.dur = q, q += 4;
    if (f & 0x10) a.size = q, q += 4;
    if (f & 0x20) a.flags = q;
    return a;
}

// The fragment's times from its moof: per track fragment the start (its tfdt when there is one, else where that
// track's last fragment ended) plus the sum of its trun sample durations; and when its first video key frame is.
static void frag_scan(Frag *f) {
    uint8_t *m = f->data;
    size_t n = f->moof_len;
    f->t_key = f->t_first = -1, f->t_end = 0;
    for (size_t o = 8; o + 8 <= n;) {
        size_t hdr, l;
        uint64_t s = box_at(m + o, n - o, &hdr);
        if (!s || o + s > n) return;
        uint8_t *tf = m + o, *tfhd = memcmp(tf + 4, "traf", 4) ? NULL : child(tf + 8, (size_t)s - 8, "tfhd", &l);
        int i = tfhd ? track_index(be32(tfhd + 12)) : -1;
        o += (size_t)s;
        if (i < 0) continue;
        struct Track *t = &tracks[i];
        TfhdAt a = tfhd_at(tfhd);
        uint32_t dur = a.dur ? be32(tfhd + a.dur) : t->dflt_dur, dflags = a.flags ? be32(tfhd + a.flags) : t->dflt_flags;
        uint8_t *tfdt = child(tf + 8, (size_t)s - 8, "tfdt", &l);
        uint64_t start = tfdt ? (tfdt[8] ? be64(tfdt + 12) : be32(tfdt + 12)) : t->next, sum = 0;
        for (size_t q = 8; q + 8 <= s;) {   // every trun of the track fragment
            size_t h2;
            uint64_t s2 = box_at(tf + q, (size_t)s - q, &h2);
            if (!s2 || q + s2 > s) break;
            uint8_t *r = tf + q;
            q += (size_t)s2;
            if (memcmp(r + 4, "trun", 4)) continue;
            uint32_t rf = be32(r + 8) & 0xffffff, cnt = be32(r + 12);
            size_t p = 16 + (rf & 0x1 ? 4 : 0) + (rf & 0x4 ? 4 : 0), fo = (rf & 0x100 ? 4 : 0) + (rf & 0x200 ? 4 : 0);
            size_t per = fo + (rf & 0x400 ? 4 : 0) + (rf & 0x800 ? 4 : 0);
            if (p + per * cnt > s2) break;
            for (uint32_t k = 0; k < cnt; k++) {
                const uint8_t *e = r + p + per * k;
                uint32_t fl = rf & 0x400 ? be32(e + fo) : k == 0 && (rf & 0x4) ? be32(r + p - 4) : dflags;
                if (t->video && f->t_key < 0 && !(fl & 0x10000)) f->t_key = (double)(start + sum) / t->scale;   // a sync sample
                sum += rf & 0x100 ? be32(e) : dur;
            }
        }
        t->next = start + sum;
        f->start[i] = start, f->has[i] = 1;
        double ts = (double)start / t->scale, te = (double)(start + sum) / t->scale;
        if (f->t_first < 0 || ts < f->t_first) f->t_first = ts;
        if (te > f->t_end) f->t_end = te;
    }
}

// Drops the oldest fragments beyond what a clip can use.
static void trim(void) {
    double newest = nfrag ? frags[nfrag - 1].t_first : 0, keep = clip_seconds() + KEEP_EXTRA_S;
    int k = 0;
    while (k < nfrag - 1 && frags[k].t_first < newest - keep) kept_bytes -= frags[k].len, free(frags[k].data), k++;
    if (k) memmove(frags, frags + k, sizeof *frags * (size_t)(nfrag - k)), nfrag -= k;
}

// A complete top-level box of the tail, at virtual position at.
static void take_box(const uint8_t *b, size_t n, uint64_t at) {
    if (!memcmp(b + 4, "moof", 4)) {
        free(open_frag.data);
        open_frag = (Frag){.data = malloc(n), .len = n, .moof_len = n, .pos = at};
        if (open_frag.data) memcpy(open_frag.data, b, n), frag_scan(&open_frag);
    } else if (!memcmp(b + 4, "mdat", 4) && open_frag.data) {
        uint8_t *d = realloc(open_frag.data, open_frag.len + n);
        if (!d) return;
        memcpy(d + open_frag.len, b, n);
        open_frag.data = d, open_frag.len += n;
        if (nfrag == capfrag) {
            Frag *g = realloc(frags, sizeof *frags * (size_t)(capfrag ? capfrag * 2 : 64));
            if (!g) return;
            frags = g, capfrag = capfrag ? capfrag * 2 : 64;
        }
        kept_bytes += open_frag.len;
        frags[nfrag++] = open_frag;
        open_frag = (Frag){0};
        trim();
    }   // sidx, mfra, free and the rest are not needed in a clip
}

static int known_box(const uint8_t *t) {
    static const char *k[] = {"moof", "mdat", "sidx", "mfra", "free", "skip", "styp", "emsg", "prft", "uuid"};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!memcmp(t, k[i], 4)) return 1;
    return 0;
}

// Moves the tail's finished boxes out: every box another complete box header follows (the writer only goes back
// to fix the header of the box it is still writing).
static void split_tail(void) {
    size_t o = 0, hdr;
    for (;;) {
        uint64_t s = box_at(tail + o, tail_len - o, &hdr);
        if (!s || !known_box(tail + o + 4) || o + s + 8 > tail_len || !known_box(tail + o + s + 4)) break;
        take_box(tail + o, (size_t)s, tail_base + o);
        o += (size_t)s;
    }
    if (o) memmove(tail, tail + o, tail_len - o), tail_len -= o, tail_base += o;
}

static int grow(uint8_t **p, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t c = *cap ? *cap : 65536;
    while (c < need) c *= 2;
    uint8_t *q = realloc(*p, c);
    if (!q) return 0;
    *p = q, *cap = c;
    return 1;
}

static void mem_write_at(const uint8_t *src, size_t n, uint64_t at) {
    plat_mutex_lock(&fl);
    if (!head_done) {   // all into head until a moof is found in it
        if (grow(&head, &head_cap, (size_t)(at + n))) {
            if (at > head_len) memset(head + head_len, 0, (size_t)(at - head_len));
            memcpy(head + at, src, n);
            if (at + n > head_len) head_len = (size_t)(at + n);
            size_t o = 0, hdr;
            for (;;) {
                uint64_t s = box_at(head + o, head_len - o, &hdr);
                if (!s || o + 8 > head_len) break;
                if (!memcmp(head + o + 4, "moof", 4)) {
                    head_done = 1;
                    tail_base = o, tail_len = 0;
                    if (grow(&tail, &tail_cap, head_len - o)) memcpy(tail, head + o, head_len - o), tail_len = head_len - o;
                    head_len = o;
                    read_moov();
                    split_tail();
                    break;
                }
                if (o + s > head_len) break;
                o += (size_t)s;
            }
        }
    } else if (at >= tail_base) {
        size_t off = (size_t)(at - tail_base);
        if (grow(&tail, &tail_cap, off + n)) {
            if (off > tail_len) memset(tail + tail_len, 0, off - tail_len);
            memcpy(tail + off, src, n);
            if (off + n > tail_len) tail_len = off + n;
            split_tail();
        }
    } else if (at + n <= head_len) {
        memcpy(head + at, src, n);   // a late change to the header
    } else {
        static int said;
        if (!said++) rt_log("clip: the writer went back to %llu, before the bytes kept (%llu)", (unsigned long long)at,
                            (unsigned long long)tail_base);
    }
    plat_mutex_unlock(&fl);
}

static void mem_reset(void) {
    plat_mutex_lock(&fl);
    for (int i = 0; i < nfrag; i++) free(frags[i].data);
    free(open_frag.data);
    open_frag = (Frag){0};
    nfrag = 0, kept_bytes = 0, head_len = 0, head_done = 0, tail_len = 0, tail_base = 0, wpos = 0, wlen = 0, ntracks = 0;
    plat_mutex_unlock(&fl);
}

// ---- IMFByteStream over the in-memory file. One object, never freed; the writer holds references while it runs.
static HRESULT STDMETHODCALLTYPE ms_qi(IMFByteStream *s, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFByteStream)) {
        *out = s;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ms_addref(IMFByteStream *s) { (void)s; return 2; }
static ULONG STDMETHODCALLTYPE ms_release(IMFByteStream *s) { (void)s; return 1; }
static HRESULT STDMETHODCALLTYPE ms_caps(IMFByteStream *s, DWORD *c) {
    (void)s;
    *c = MFBYTESTREAM_IS_WRITABLE | MFBYTESTREAM_IS_SEEKABLE | MFBYTESTREAM_IS_READABLE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_getlen(IMFByteStream *s, QWORD *n) { (void)s; *n = wlen; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_setlen(IMFByteStream *s, QWORD n) { (void)s; wlen = n; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_getpos(IMFByteStream *s, QWORD *p) { (void)s; *p = wpos; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_setpos(IMFByteStream *s, QWORD p) { (void)s; wpos = p; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_eos(IMFByteStream *s, BOOL *e) { (void)s; *e = wpos >= wlen; return S_OK; }
// Reads back what is still held (the header or the tail); anything else reads as zeros.
static HRESULT STDMETHODCALLTYPE ms_read(IMFByteStream *s, BYTE *b, ULONG n, ULONG *got) {
    (void)s;
    plat_mutex_lock(&fl);
    for (ULONG i = 0; i < n; i++) {
        uint64_t p = wpos + i;
        b[i] = p < head_len && (!head_done || p < tail_base) ? head[p]
             : head_done && p >= tail_base && p - tail_base < tail_len ? tail[p - tail_base] : 0;
    }
    plat_mutex_unlock(&fl);
    ULONG k = wpos + n > wlen ? (ULONG)(wlen > wpos ? wlen - wpos : 0) : n;
    wpos += k;
    *got = k;
    return S_OK;
}
static HRESULT done_async(IMFAsyncCallback *cb, IUnknown *state) {
    IMFAsyncResult *r;
    HRESULT hr = mf.CreateAsyncResult(NULL, cb, state, &r);
    if (FAILED(hr)) return hr;
    IMFAsyncResult_SetStatus(r, S_OK);
    hr = mf.InvokeCallback(r);
    IMFAsyncResult_Release(r);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ms_bread(IMFByteStream *s, BYTE *b, ULONG n, IMFAsyncCallback *cb, IUnknown *st) {
    ULONG got;
    ms_read(s, b, n, &got);
    last_write = got;
    return done_async(cb, st);
}
static HRESULT STDMETHODCALLTYPE ms_eread(IMFByteStream *s, IMFAsyncResult *r, ULONG *got) { (void)s, (void)r; *got = last_write; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_write(IMFByteStream *s, const BYTE *b, ULONG n, ULONG *put) {
    (void)s;
    mem_write_at(b, n, wpos);
    wpos += n;
    if (wpos > wlen) wlen = wpos;
    *put = n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_bwrite(IMFByteStream *s, const BYTE *b, ULONG n, IMFAsyncCallback *cb, IUnknown *st) {
    ULONG put;
    ms_write(s, b, n, &put);
    last_write = put;
    return done_async(cb, st);
}
static HRESULT STDMETHODCALLTYPE ms_ewrite(IMFByteStream *s, IMFAsyncResult *r, ULONG *put) { (void)s, (void)r; *put = last_write; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_seek(IMFByteStream *s, MFBYTESTREAM_SEEK_ORIGIN o, LONGLONG off, DWORD flags, QWORD *cur) {
    (void)s, (void)flags;
    wpos = o == msoBegin ? (QWORD)off : (QWORD)((LONGLONG)wpos + off);
    if (cur) *cur = wpos;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_flush(IMFByteStream *s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_close(IMFByteStream *s) { (void)s; return S_OK; }
static IMFByteStreamVtbl ms_vt = {ms_qi, ms_addref, ms_release, ms_caps, ms_getlen, ms_setlen, ms_getpos, ms_setpos, ms_eos,
                                  ms_read, ms_bread, ms_eread, ms_write, ms_bwrite, ms_ewrite, ms_seek, ms_flush, ms_close};
static IMFByteStream ms = {&ms_vt};

// ---- the writer (recorder thread only)
static IMFSinkWriter *wr;
static DWORD vs, as;
static int vw, vh, vbitrate;
static uint64_t t0_ns;
static uint64_t audio_n;     // frames written to the audio stream
static double a_frac;        // resampling position between input frames
static _Atomic uint64_t frames_in, frames_dropped;
static char last_error[96];

static void set_size(IMFMediaType *t, const GUID *key, uint32_t a, uint32_t b) { IMFMediaType_SetUINT64(t, key, (uint64_t)a << 32 | b); }

static void video_attrs(IMFMediaType *t, int w, int h) {
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    set_size(t, &MF_MT_FRAME_SIZE, (uint32_t)w, (uint32_t)h);
    set_size(t, &MF_MT_FRAME_RATE, FPS, 1);
    set_size(t, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    IMFMediaType_SetUINT32(t, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    IMFMediaType_SetUINT32(t, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
}

static IMFMediaBuffer *v_last;   // the last frame written (write_frame), for the slots after it that get none
static int64_t v_slot = -1;      // its slot
static void writer_stop(void) {
    if (v_last) IMFMediaBuffer_Release(v_last), v_last = NULL;
    v_slot = -1;
    if (!wr) return;
    IMFSinkWriter_Finalize(wr);
    IMFSinkWriter_Release(wr);
    wr = NULL;
    mem_reset();
}

#define TRY(x, what)                                                                                                   \
    do {                                                                                                               \
        HRESULT hr_ = (x);                                                                                             \
        if (FAILED(hr_)) {                                                                                             \
            snprintf(last_error, sizeof last_error, "%s failed (0x%08lx)", what, (unsigned long)hr_);                 \
            rt_log("clip: %s", last_error);                                                                            \
            goto fail;                                                                                                 \
        }                                                                                                              \
    } while (0)

static int writer_start(int w, int h) {
    IMFAttributes *a = NULL;
    IMFMediaType *vo = NULL, *vi = NULL, *ao = NULL, *ai = NULL;
    mem_reset();
    vbitrate = (int)((double)w * h * FPS * 0.1);   // about 12 Mbit/s at 1920 x 1080
    if (vbitrate < 4000000) vbitrate = 4000000;
    if (vbitrate > 30000000) vbitrate = 30000000;
    TRY(mf.CreateAttributes(&a, 4), "the writer's attributes");
    IMFAttributes_SetUINT32(a, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFAttributes_SetUINT32(a, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);   // one thread feeds both streams
    IMFAttributes_SetGUID(a, &MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_FMPEG4);
    TRY(mf.CreateSinkWriterFromURL(NULL, &ms, a, &wr), "creating the MP4 writer");

    TRY(mf.CreateMediaType(&vo), "a media type");
    video_attrs(vo, w, h);
    IMFMediaType_SetGUID(vo, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(vo, &MF_MT_AVG_BITRATE, (UINT32)vbitrate);
    IMFMediaType_SetUINT32(vo, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    IMFMediaType_SetUINT32(vo, &MF_MT_MAX_KEYFRAME_SPACING, GOP);
    TRY(IMFSinkWriter_AddStream(wr, vo, &vs), "the video stream");
    TRY(mf.CreateMediaType(&vi), "a media type");
    video_attrs(vi, w, h);
    IMFMediaType_SetGUID(vi, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT32(vi, &MF_MT_DEFAULT_STRIDE, (UINT32)w);
    TRY(IMFSinkWriter_SetInputMediaType(wr, vs, vi, NULL), "the H.264 encoder for NV12");
    ICodecAPI *api = NULL;
    if (SUCCEEDED(IMFSinkWriter_GetServiceForStream(wr, vs, &clip_no_service, &IID_ICodecAPI, (void **)&api)) && api) {
        VARIANT v;
        memset(&v, 0, sizeof v);
        v.vt = VT_UI4, v.ulVal = GOP;
        ICodecAPI_SetValue(api, &CLIP_GOP_SIZE, &v);
        v.ulVal = 0;
        ICodecAPI_SetValue(api, &CLIP_B_FRAMES, &v);
        ICodecAPI_Release(api);
    }

    TRY(mf.CreateMediaType(&ao), "a media type");
    IMFMediaType_SetGUID(ao, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(ao, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(ao, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(ao, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
    IMFMediaType_SetUINT32(ao, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    IMFMediaType_SetUINT32(ao, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   // 192 kbit/s
    TRY(IMFSinkWriter_AddStream(wr, ao, &as), "the audio stream");
    TRY(mf.CreateMediaType(&ai), "a media type");
    IMFMediaType_SetGUID(ai, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(ai, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(ai, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(ai, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
    IMFMediaType_SetUINT32(ai, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    IMFMediaType_SetUINT32(ai, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    IMFMediaType_SetUINT32(ai, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_RATE * 4);
    TRY(IMFSinkWriter_SetInputMediaType(wr, as, ai, NULL), "the AAC encoder");
    TRY(IMFSinkWriter_BeginWriting(wr), "starting the writer");
    IMFAttributes_Release(a), IMFMediaType_Release(vo), IMFMediaType_Release(vi), IMFMediaType_Release(ao), IMFMediaType_Release(ai);
    vw = w, vh = h;
    audio_n = 0, a_frac = 0, v_slot = -1;
    a_rd = atomic_load(&a_wr);   // sound from before the start is not wanted
    last_error[0] = 0;
    rt_log("clip: recording %dx%d at %d fps, %.1f Mbit/s, with sound", w, h, FPS, vbitrate / 1e6);
    return 1;
fail:
    if (a) IMFAttributes_Release(a);
    if (vo) IMFMediaType_Release(vo);
    if (vi) IMFMediaType_Release(vi);
    if (ao) IMFMediaType_Release(ao);
    if (ai) IMFMediaType_Release(ai);
    if (wr) IMFSinkWriter_Release(wr), wr = NULL;
    mem_reset();
    return 0;
}

static int write_sample(DWORD stream, IMFMediaBuffer *b, int64_t t100, int64_t d100) {
    IMFSample *s = NULL;
    HRESULT hr = mf.CreateSample(&s);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(s, b);
    if (SUCCEEDED(hr)) IMFSample_SetSampleTime(s, t100), IMFSample_SetSampleDuration(s, d100), hr = IMFSinkWriter_WriteSample(wr, stream, s);
    if (s) IMFSample_Release(s);
    IMFMediaBuffer_Release(b);
    if (FAILED(hr)) snprintf(last_error, sizeof last_error, "writing a sample failed (0x%08lx)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

// ---- picture: 4-byte pixels to NV12 (BT.709, limited range), each 2 x 2 block sharing its chroma
static uint8_t pq_lut_ready, hdr_r[1024], hdr_g[1024], hdr_b[1024];
// HDR10 frames: sRGB bytes per PQ code, from vk_hdr_to_srgb8 on grey pixels (the Rec. 2020 to Rec. 709 matrix maps
// grey to grey), so a frame's channels convert through a table instead of a power function per pixel.
static void pq_lut(void) {
    if (pq_lut_ready) return;
    for (int i = 0; i < 1024; i++) {
        uint32_t v = (uint32_t)i | (uint32_t)i << 10 | (uint32_t)i << 20 | 3u << 30;
        uint8_t px[4], out[4];
        memcpy(px, &v, 4);
        vk_hdr_to_srgb8(px, out, 1, 0);
        hdr_r[i] = out[0], hdr_g[i] = out[1], hdr_b[i] = out[2];
    }
    pq_lut_ready = 1;
}

static inline void rgb_of(const uint8_t *p, int kind, int *r, int *g, int *b) {
    if (kind == CLIP_PX_BGRA) *r = p[2], *g = p[1], *b = p[0];
    else if (kind == CLIP_PX_RGBA) *r = p[0], *g = p[1], *b = p[2];
    else {
        uint32_t v;
        memcpy(&v, p, 4);
        int lo = (int)(v & 1023), mid = (int)(v >> 10 & 1023), hi = (int)(v >> 20 & 1023);
        if (kind == CLIP_PX_PQ_A2R10) *r = hdr_r[hi], *g = hdr_g[mid], *b = hdr_b[lo];
        else *r = hdr_r[lo], *g = hdr_g[mid], *b = hdr_b[hi];
    }
}

static void to_nv12(const struct Pending *f, uint8_t *dst) {
    int w = f->w & ~1, h = f->h & ~1;
    uint8_t *uv = dst + (size_t)w * h;
    if (f->kind >= CLIP_PX_PQ_A2B10) pq_lut();
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = f->px + (size_t)y * f->stride, *r1 = r0 + f->stride;
        uint8_t *y0 = dst + (size_t)y * w, *y1 = y0 + w, *c = uv + (size_t)(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int R[4], G[4], B[4];
            rgb_of(r0 + 4 * x, f->kind, &R[0], &G[0], &B[0]);
            rgb_of(r0 + 4 * x + 4, f->kind, &R[1], &G[1], &B[1]);
            rgb_of(r1 + 4 * x, f->kind, &R[2], &G[2], &B[2]);
            rgb_of(r1 + 4 * x + 4, f->kind, &R[3], &G[3], &B[3]);
            y0[x] = (uint8_t)(16 + ((47 * R[0] + 157 * G[0] + 16 * B[0] + 128) >> 8));
            y0[x + 1] = (uint8_t)(16 + ((47 * R[1] + 157 * G[1] + 16 * B[1] + 128) >> 8));
            y1[x] = (uint8_t)(16 + ((47 * R[2] + 157 * G[2] + 16 * B[2] + 128) >> 8));
            y1[x + 1] = (uint8_t)(16 + ((47 * R[3] + 157 * G[3] + 16 * B[3] + 128) >> 8));
            int r = R[0] + R[1] + R[2] + R[3], g = G[0] + G[1] + G[2] + G[3], b = B[0] + B[1] + B[2] + B[3];
            c[x] = (uint8_t)(128 + ((-26 * r - 87 * g + 113 * b + 512) >> 10));
            c[x + 1] = (uint8_t)(128 + ((113 * r - 103 * g - 10 * b + 512) >> 10));
        }
    }
}

static int64_t slot_t(int64_t k) { return k * 10000000 / FPS; }   // a slot's start in 100 ns units

static void write_frame(const struct Pending *f) {
    int w = f->w & ~1, h = f->h & ~1;
    if (wr && (w != vw || h != vh)) writer_stop();   // a new size starts a new recording
    if (!wr) {
        if (!writer_start(w, h)) return;
        t0_ns = f->ns;
    }
    MF_SINK_WRITER_STATISTICS st = {.cb = sizeof st};
    if (SUCCEEDED(IMFSinkWriter_GetStatistics(wr, vs, &st)) && st.qwNumSamplesReceived > st.qwNumSamplesEncoded + 20) {
        atomic_fetch_add(&frames_dropped, 1);   // the encoder is behind: this frame is left out
        return;
    }
    // The video runs at a constant 60 frames a second: a frame goes in the slot of its time, and the slots the game
    // gave none (it ran slower, or a frame was left out) repeat the one before. The writer times its samples by their
    // durations, so a timeline with gaps would drift from the sound.
    int64_t k = (int64_t)((f->ns - t0_ns + FRAME_NS / 2) / FRAME_NS);
    if (k <= v_slot && v_slot >= 0) return;   // a second frame in the same slot
    for (int64_t j = v_slot + 1; v_last && j < k && j < v_slot + 2 * FPS; j++) {
        IMFMediaBuffer_AddRef(v_last);
        write_sample(vs, v_last, slot_t(j), slot_t(j + 1) - slot_t(j));
    }
    IMFMediaBuffer *b;
    DWORD n = (DWORD)((size_t)w * h * 3 / 2);
    if (FAILED(mf.CreateMemoryBuffer(n, &b))) return;
    BYTE *p;
    if (FAILED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) { IMFMediaBuffer_Release(b); return; }
    to_nv12(f, p);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, n);
    if (v_last) IMFMediaBuffer_Release(v_last);
    v_last = b, v_slot = k;
    IMFMediaBuffer_AddRef(b);
    if (write_sample(vs, b, slot_t(k), slot_t(k + 1) - slot_t(k))) atomic_fetch_add(&frames_in, 1);
}

// ---- sound to the writer: what the ring holds, at 48 kHz, with silence where the device sent nothing
static void write_audio_frames(const int16_t *pcm, uint32_t n) {
    for (uint32_t o = 0; o < n;) {
        uint32_t k = n - o > AUDIO_CHUNK ? AUDIO_CHUNK : n - o;
        IMFMediaBuffer *b;
        if (FAILED(mf.CreateMemoryBuffer(k * 4, &b))) return;
        BYTE *p;
        if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
            if (pcm) memcpy(p, pcm + 2 * o, k * 4);
            else memset(p, 0, k * 4);
            IMFMediaBuffer_Unlock(b);
        }
        IMFMediaBuffer_SetCurrentLength(b, k * 4);
        write_sample(as, b, (int64_t)(audio_n * 10000000 / AUDIO_RATE), (int64_t)((uint64_t)k * 10000000 / AUDIO_RATE));
        audio_n += k, o += k;
    }
}

static void drain_audio(uint64_t now_ns) {
    if (!wr) return;
    uint64_t w = atomic_load_explicit(&a_wr, memory_order_acquire), sp = atomic_load(&a_stamp_pos), sn = atomic_load(&a_stamp_ns);
    int rate = atomic_load(&a_rate);
    if (w - a_rd > AUDIO_RING) a_rd = w - AUDIO_RING;   // the recorder fell behind by more than the ring
    if (w > a_rd && rate > 0) {
        // where the oldest unread frame belongs on the clip's clock, against where the stream has got to
        double at_ns = (double)sn - (double)(sp - a_rd) * 1e9 / rate;
        int64_t want = (int64_t)((at_ns - (double)t0_ns) * AUDIO_RATE / 1e9);
        if (want < 0) {   // sent before the recording began
            uint64_t skip = (uint64_t)((double)-want * rate / AUDIO_RATE);
            a_rd += skip < w - a_rd ? skip : w - a_rd;
            want = 0;
        }
        if (want > (int64_t)audio_n + AUDIO_SLACK) write_audio_frames(NULL, (uint32_t)(want - (int64_t)audio_n));
        else if (want < (int64_t)audio_n - AUDIO_SLACK) {   // ahead of the device: the excess input goes
            uint64_t skip = (uint64_t)((double)((int64_t)audio_n - want) * rate / AUDIO_RATE);
            a_rd += skip < w - a_rd ? skip : w - a_rd;
        }
        static int16_t out[AUDIO_CHUNK * 8 * 2];
        while (a_rd < w) {
            uint32_t n = 0;
            if (rate == AUDIO_RATE) {
                while (a_rd < w && n < AUDIO_CHUNK * 8) {
                    uint64_t k = a_rd % AUDIO_RING;
                    out[2 * n] = aring[2 * k], out[2 * n + 1] = aring[2 * k + 1];
                    n++, a_rd++;
                }
            } else {   // another device rate: linear steps through the input
                double step = (double)rate / AUDIO_RATE;
                while (a_rd + 1 < w && n < AUDIO_CHUNK * 8) {
                    uint64_t k = a_rd % AUDIO_RING, k1 = (a_rd + 1) % AUDIO_RING;
                    out[2 * n] = (int16_t)(aring[2 * k] + (aring[2 * k1] - aring[2 * k]) * a_frac);
                    out[2 * n + 1] = (int16_t)(aring[2 * k + 1] + (aring[2 * k1 + 1] - aring[2 * k + 1]) * a_frac);
                    n++;
                    a_frac += step;
                    while (a_frac >= 1.0) a_frac -= 1.0, a_rd++;
                }
                if (!n) break;
            }
            write_audio_frames(out, n);
        }
    }
    // nothing from the device for a while (no output, or a pause): silence up to a tenth of a second ago
    int64_t now_n = (int64_t)((double)(now_ns - t0_ns) * AUDIO_RATE / 1e9) - AUDIO_RATE / 10;
    if (now_n > (int64_t)audio_n + AUDIO_RATE / 4) write_audio_frames(NULL, (uint32_t)(now_n - (int64_t)audio_n));
}

// ---- saving
// Copies fragment f to out (at position out_pos of the clip) with a tfdt in every track fragment: its decode time
// counted from the clip's start (base, per track). With cut (the clip's first fragment) the samples that start before
// cut[track] are left out of the truns (their bytes stay in the mdat, unreferenced): the clip starts on its key frame.
// Moves the data offsets for the boxes added and the samples left out. Returns the bytes written, at most
// f->len + TFDT_ROOM.
#define TFDT_ROOM (4 * 20)
static size_t emit_frag(uint8_t *out, const Frag *f, uint64_t out_pos, const uint64_t *base, const uint64_t *cut) {
    const uint8_t *m = f->data;
    size_t n = f->moof_len, w = 8, hdr, l;
    memcpy(out, m, 8);
    for (size_t o = 8; o + 8 <= n;) {
        uint64_t s = box_at(m + o, n - o, &hdr);
        if (!s || o + s > n) break;
        const uint8_t *b = m + o;
        o += (size_t)s;
        uint8_t *tfhd = memcmp(b + 4, "traf", 4) ? NULL : child((uint8_t *)b + 8, (size_t)s - 8, "tfhd", &l);
        int i = tfhd ? track_index(be32(tfhd + 12)) : -1;
        if (i < 0 || !f->has[i]) {
            memcpy(out + w, b, (size_t)s), w += (size_t)s;
            continue;
        }
        TfhdAt a = tfhd_at(tfhd);
        uint32_t ddur = a.dur ? be32(tfhd + a.dur) : tracks[i].dflt_dur, dsize = a.size ? be32(tfhd + a.size) : 0;
        size_t traf = w;
        w += 8;
        memcpy(out + w, tfhd, l), w += l;   // tfhd first, then the new tfdt, then the rest without any old tfdt
        size_t tfdt = w;
        put32(out + w, 20), memcpy(out + w + 4, "tfdt", 4), put32(out + w + 8, 0x01000000), w += 20;
        uint64_t tt = f->start[i], first = 0;   // the time of the sample at hand; that of the first one kept
        int any = 0;
        for (size_t q = 8; q + 8 <= s;) {
            size_t h2;
            uint64_t s2 = box_at(b + q, (size_t)s - q, &h2);
            if (!s2 || q + s2 > s) break;
            const uint8_t *r = b + q;
            q += (size_t)s2;
            if (!memcmp(r + 4, "tfhd", 4) || !memcmp(r + 4, "tfdt", 4)) continue;
            if (memcmp(r + 4, "trun", 4)) {
                memcpy(out + w, r, (size_t)s2), w += (size_t)s2;
                continue;
            }
            uint32_t rf = be32(r + 8) & 0xffffff, cnt = be32(r + 12), k = 0;
            size_t p = 16 + (rf & 0x1 ? 4 : 0) + (rf & 0x4 ? 4 : 0), so = rf & 0x100 ? 4 : 0;
            size_t per = so + (rf & 0x200 ? 4 : 0) + (rf & 0x400 ? 4 : 0) + (rf & 0x800 ? 4 : 0);
            if (p + per * cnt > s2) {
                memcpy(out + w, r, (size_t)s2), w += (size_t)s2;
                continue;
            }
            uint64_t skipped = 0;   // bytes of the samples left out
            for (; cut && k < cnt && tt < cut[i]; k++) {
                const uint8_t *e = r + p + per * k;
                tt += rf & 0x100 ? be32(e) : ddur;
                skipped += rf & 0x200 ? be32(e + so) : dsize;
            }
            if (!any && k < cnt) first = tt, any = 1;
            for (uint32_t j = k; j < cnt; j++) tt += rf & 0x100 ? be32(r + p + per * j) : ddur;
            uint8_t *nr = out + w;
            memcpy(nr, r, p);
            memcpy(nr + p, r + p + per * k, per * (cnt - k));
            put32(nr, (uint32_t)(p + per * (cnt - k)));
            put32(nr + 12, cnt - k);
            if (rf & 0x1) put32(nr + 16, be32(nr + 16) + (uint32_t)skipped);
            if ((rf & 0x4) && k) put32(nr + 16 + (rf & 0x1 ? 4 : 0), be32(nr + 16 + (rf & 0x1 ? 4 : 0)) & ~0x10000u);
            w += p + per * (cnt - k);
        }
        if (!any) first = tt;
        put64(out + tfdt + 12, first > base[i] ? first - base[i] : 0);
        put32(out + traf, (uint32_t)(w - traf)), memcpy(out + traf + 4, "traf", 4);
    }
    put32(out, (uint32_t)w);
    // the mdat moved: by (w - moof_len) inside the fragment, and by (out_pos - pos) in the file
    int64_t in_frag = (int64_t)w - (int64_t)n, in_file = (int64_t)out_pos - (int64_t)f->pos + in_frag;
    for (size_t o = 8; o + 8 <= w;) {
        uint64_t s = box_at(out + o, w - o, &hdr);
        if (!s || o + s > w) break;
        uint8_t *b = out + o;
        o += (size_t)s;
        if (memcmp(b + 4, "traf", 4)) continue;
        uint8_t *tfhd = child(b + 8, (size_t)s - 8, "tfhd", &l);
        if (!tfhd) continue;
        TfhdAt a = tfhd_at(tfhd);
        if (a.base) {   // an absolute base: the trun offsets stay relative to it
            put64(tfhd + a.base, (uint64_t)((int64_t)be64(tfhd + a.base) + in_file));
            continue;
        }
        for (size_t q = 8; q + 8 <= s;) {   // offsets from the moof's start
            size_t h2;
            uint64_t s2 = box_at(b + q, (size_t)s - q, &h2);
            if (!s2 || q + s2 > s) break;
            uint8_t *r = b + q;
            if (!memcmp(r + 4, "trun", 4) && (be32(r + 8) & 0x1)) put32(r + 16, (uint32_t)((int32_t)be32(r + 16) + in_frag));
            q += (size_t)s2;
        }
    }
    memcpy(out + w, m + n, f->len - n);
    return w + (f->len - n);
}

// The header with its durations set to the clip's (mvhd, and mehd when there is one), so players show its length.
static void set_durations(uint8_t *h, size_t n, double secs) {
    size_t ml, l;
    uint8_t *moov = child(h, n, "moov", &ml), *p;
    if (!moov) return;
    uint32_t scale = 0;
    if ((p = child(moov + 8, ml - 8, "mvhd", &l)) && l >= 32) {
        if (p[8]) scale = be32(p + 28), put64(p + 32, (uint64_t)(secs * scale));
        else scale = be32(p + 20), put32(p + 24, (uint32_t)(secs * scale));
    }
    uint8_t *mv = child(moov + 8, ml - 8, "mvex", &l);
    size_t mvl = l;
    if (scale && mv && (p = child(mv + 8, mvl - 8, "mehd", &l))) {
        if (p[8]) put64(p + 12, (uint64_t)(secs * scale));
        else put32(p + 12, (uint32_t)(secs * scale));
    }
}

static int write_file(const char *path, const uint8_t *d, size_t n) {
    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH)) return 0;
    FILE *f = NULL;
    if (_wfopen_s(&f, w, L"wb") || !f) return 0;
    size_t put = fwrite(d, 1, n, f);
    return fclose(f) == 0 && put == n;
}

static void clips_dir(char *out, size_t n) {
    char exe[1024];
    out[0] = 0;
    if (!plat_exe_path(exe, sizeof exe)) { snprintf(out, n, "clips"); return; }
    char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
    if (back > cut) cut = back;
    if (cut) *cut = 0;
    snprintf(out, n, "%s/clips", exe);
}

int clip_open_folder(void) {
    char dir[1024];
    clips_dir(dir, sizeof dir);
    plat_file_mkdir_p(dir);
    return plat_open_folder(dir);
}

static void saver(void *arg) {
    uint64_t press = (uint64_t)(uintptr_t)arg;
    double secs = clip_seconds(), at;
    // wait for the fragment holding the moment the key was pressed: it is complete once the one after it begins
    for (uint64_t until = plat_now_ns() + SAVE_WAIT_NS;;) {
        plat_mutex_lock(&fl);
        at = (double)((int64_t)press - (int64_t)t0_ns) / 1e9;
        int ready = nfrag && frags[nfrag - 1].t_end >= at;
        plat_mutex_unlock(&fl);
        if (ready || plat_now_ns() > until) break;
        plat_sleep_ms(50);
    }
    plat_mutex_lock(&fl);
    int s = -1, e = nfrag;
    for (int i = 0; i < nfrag; i++) {   // the last key frame at or before the start, else the first one there is
        if (frags[i].t_key < 0) continue;
        if (frags[i].t_key <= at - secs || s < 0) s = i;
        if (frags[i].t_key > at - secs) break;
    }
    if (s < 0 || !head_done) {
        plat_mutex_unlock(&fl);
        dbg_toast_show("Clip: nothing recorded yet");
        return;
    }
    while (e > s + 1 && frags[e - 1].t_first > at) e--;   // what came after the key press
    size_t total = head_len;
    for (int i = s; i < e; i++) total += frags[i].len + TFDT_ROOM;
    uint8_t *d = malloc(total);
    if (!d) {
        plat_mutex_unlock(&fl);
        dbg_toast_show("Clip: not enough memory to save");
        return;
    }
    // the clip starts on that key frame: every track counts from it, and what comes before it in that fragment goes
    double t0 = frags[s].t_key;
    uint64_t base[4];
    for (int i = 0; i < ntracks; i++) base[i] = (uint64_t)(t0 * tracks[i].scale + 0.5);
    memcpy(d, head, head_len);
    size_t o = head_len;
    for (int i = s; i < e; i++) o += emit_frag(d + o, &frags[i], o, base, i == s ? base : NULL);
    double dur = frags[e - 1].t_end - t0;
    plat_mutex_unlock(&fl);
    set_durations(d, head_len, dur);

    char dir[1024], path[1200], name[64];
    clips_dir(dir, sizeof dir);
    plat_file_mkdir_p(dir);
    struct tm tm;
    plat_local_time(&tm);
    strftime(name, sizeof name, "OOT-DEBUG-MENU %Y-%m-%d %H-%M-%S", &tm);
    snprintf(path, sizeof path, "%s/%s.mp4", dir, name);
    int ok = write_file(path, d, o);
    free(d);
    char t[160];
    if (ok) snprintf(t, sizeof t, "Clip saved (%.0f s): clips/%s.mp4", dur, name);
    else snprintf(t, sizeof t, "Clip: could not write clips/%s.mp4", name);
    dbg_toast_show(t);
    rt_log("clip: %s %s (%.1f s, %.1f MB)", ok ? "saved" : "could not write", path, dur, o / 1048576.0);
}

int clip_save(void) {
    if (!clip_enabled()) {
        dbg_toast_show("Clip recording is off (Debug tab, Clips)");
        return 0;
    }
    if (!atomic_load(&started) || mf_state <= 0) {
        dbg_toast_show(mf_state < 0 ? "Clips are not available on this system" : "Clip: nothing recorded yet");
        return 0;
    }
    plat_mutex_lock(&lk);
    save_asked = 1, save_ns = plat_now_ns();
    plat_cond_signal(&wake);
    plat_mutex_unlock(&lk);
    dbg_toast_show("Saving clip...");
    return 1;
}

// ---- the recorder
static void recorder(void *arg) {
    (void)arg;
    plat_mutex_init(&fl);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!mf_load()) {
        rt_log("clip: off: %s", mf_why);
        return;
    }
    rt_log("clip: recorder ready");
    for (;;) {
        plat_mutex_lock(&lk);
        if (pending.buf < 0 && !save_asked) plat_cond_wait_ms(&wake, &lk, 20);
        struct Pending f = pending;
        pending.buf = -1;
        int save = save_asked;
        uint64_t sns = save_ns;
        save_asked = 0;
        plat_mutex_unlock(&lk);
        if (atomic_load(&enabled) != 1) {   // switched off: the recording and what it kept go
            if (f.buf >= 0) vk_clip_release(f.buf);
            writer_stop();
            continue;
        }
        if (f.buf >= 0) {
            write_frame(&f);
            vk_clip_release(f.buf);
        }
        drain_audio(plat_now_ns());
        if (save && wr && !plat_thread_spawn(saver, (void *)(uintptr_t)sns, "clip saver", 0))
            dbg_toast_show("Clip: could not start saving");
    }
}

void clip_status(char *out, int n) {
    if (!clip_enabled()) snprintf(out, (size_t)n, "off");
    else if (mf_state < 0) snprintf(out, (size_t)n, "not available: %s", mf_why[0] ? mf_why : "Media Foundation did not load");
    else if (last_error[0]) snprintf(out, (size_t)n, "stopped: %s", last_error);
    else if (!wr) snprintf(out, (size_t)n, "starting...");
    else {
        plat_mutex_lock(&fl);
        double kept = nfrag ? frags[nfrag - 1].t_first - frags[0].t_first : 0;
        size_t mb = kept_bytes >> 20;
        plat_mutex_unlock(&fl);
        snprintf(out, (size_t)n, "%dx%d, %d fps, %.0f Mbit/s; %.0f s kept (%zu MB)%s", vw, vh, FPS, vbitrate / 1e6, kept, mb,
                 atomic_load(&frames_dropped) ? ", some frames skipped" : "");
    }
}
