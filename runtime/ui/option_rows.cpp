// Two-pane settings pages (option_rows.h): rows on the left, the selected or hovered row's description on the right.
//
// Contains code Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
// The rows read and write cfg keys, take their text from the menu catalog (menu_text), and show the default, "after
// restart" and environment notes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner, SetFocusID, NavCursorVisible

#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "menu_sound.h"
// host.h pulls in cpu.h, which is C only: the one function needed from it, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static const ImU32 kGrey = IM_COL32(110, 110, 102, 255);

static float g_scale = 1;
static bool g_focus_req, g_take_first;   // opt_focus_first: requested / this page's first row takes it
static char g_focus_key[64];             // opt_focus_key: the row that takes the cursor when it is next drawn
static bool g_held;                      // the last row's left / right has been held a while (steppers go faster)
static float g_bottom;                   // opt_reserve_bottom: kept free under the panes
static bool g_disable_next;              // opt_disable_next: the next row is greyed out with g_disable_note
static const char *g_disable_note;
static bool g_lit_next;                  // the next row is lit as if selected: its list is open (opt_text_choice)

// opt_focus_resume: each page's row that last had the cursor, by key. The page's scroll position needs no keeping: its
// child window holds it while the menu is closed, and only opt_focus_first scrolls it back to the top.
static struct Place { ImGuiID page; char key[64]; } g_places[8];
static bool g_resume_req, g_resume;      // requested / this page looks for its kept row while its rows are drawn
static char g_resume_key[64];
static ImGuiID g_resume_fallback;        // the page's first row wholly in view, should the kept one not be

// The item takes the cursor. The window that has just appeared (the menu opening) has asked for its first item to
// take it, which is the first tab; that request, not yet applied, is dropped, or it would win at the next frame and
// leave the cursor on the tab bar.
static void take_cursor(ImGuiID id) {
    ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGuiContext &g = *GImGui;
    g.NavInitRequest = false;
    g.NavInitResult.ID = 0;
}

static Place *place_of(ImGuiID page, bool add) {
    for (Place &p : g_places)
        if (p.page == page) return &p;
    if (!add) return nullptr;
    for (Place &p : g_places)
        if (!p.page) return p.page = page, p.key[0] = 0, &p;
    return &g_places[0];   // more pages than places: the first one's is given up
}

// The rows' cursor, drawn once all rows are (opt_end), its glow in the layer under the rows and its frame in the one
// over them: the selected row's box, else the hovered one's (alpha: fainter for a hovered row, and for one the
// environment fixes).
static struct { bool on; ImVec2 a, b; float alpha; } g_cursor;
// Room left on each side of a row for its cursor, which is drawn outside the row's box.
static float row_margin(void) { return ceilf(skin_cursor_reach(g_scale) * 0.85f); }

// The description pane: this frame's hovered and selected rows, the page's first row, and the last one shown (of the
// page g_page: another page starts from its first row's, however it was opened).
struct Desc { char text[UI_DESC_BYTES]; };
static Desc g_hover, g_focus, g_first, g_shown;
static ImGuiID g_page;

// disabled: the note of a row opt_disable_next greyed out, else NULL.
static void desc_set(Desc &d, const char *desc, const char *key, const CfgMeta *m, const char *disabled) {
    int n = snprintf(d.text, sizeof d.text, "%s", desc);
    if (n < 0 || n >= (int)sizeof d.text) return;
    if (disabled)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", disabled);
    else if (cfg_from_env(key))
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s %s", menu_text(UI_MENU_SET_BY_ENV), key);
    else if (m && !m->live)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", menu_text(UI_MENU_AFTER_RESTART));
}

void opt_focus_first(void) { g_focus_req = true; }
void opt_focus_resume(void) { g_resume_req = true; }

void opt_focus_key(const char *key) { snprintf(g_focus_key, sizeof g_focus_key, "%s", key); }

void opt_reserve_bottom(float h) { g_bottom = h; }
float opt_bottom(void) { return g_bottom; }

void opt_disable_next(UiTextId note) { opt_disable_next_text(menu_text(note)); }

void opt_disable_next_text(const char *note) {
    g_disable_next = true;
    g_disable_note = note;
}

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    g_hover.text[0] = g_focus.text[0] = g_first.text[0] = 0;
    if (ImGuiID page = ImGui::GetID(id); page != g_page) g_page = page, g_shown.text[0] = 0;
    // opt_focus_resume: a page shown for the first time has no kept row, so it starts from its first one
    g_resume = false;
    if (g_resume_req) {
        const Place *pl = place_of(g_page, false);
        if (pl && pl->key[0]) {
            g_resume = true, g_take_first = false, g_resume_fallback = 0;
            snprintf(g_resume_key, sizeof g_resume_key, "%s", pl->key);
        } else {
            g_take_first = true;
        }
        g_resume_req = false;
    }
    if (g_take_first || g_resume) ImGui::NavMoveRequestCancel();   // this frame's move goes to that row instead
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.58f, avail.y), ImGuiChildFlags_NavFlattened);
    // the rows sit far enough apart that the selected row's cursor frame clears its neighbours' boxes
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 9 * scale));
    ImGui::Dummy(ImVec2(0, row_margin()));   // and the first row's cursor stays inside the pane
    g_cursor.on = false;
    // three layers: the cursor's glow under the rows, the rows, the cursor's frame over them (opt_end draws the cursor)
    ImGui::GetWindowDrawList()->ChannelsSplit(3);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
}

void opt_end(void) {
    ImGui::Dummy(ImVec2(0, row_margin()));   // the last row's cursor stays inside the pane
    if (g_resume) {   // the kept row is gone or out of view: the first row in view takes the cursor, the scroll stays
        g_resume = false;
        if (g_resume_fallback) {
            take_cursor(g_resume_fallback);
        }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_cursor.on) {
        dl->ChannelsSetCurrent(0);
        skin_cursor_glow(dl, g_cursor.a, g_cursor.b, g_scale, g_cursor.alpha);
        dl->ChannelsSetCurrent(2);
        skin_cursor_frame(dl, g_cursor.a, g_cursor.b, g_scale, g_cursor.alpha);
    }
    dl->ChannelsMerge();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // the hovered row (mouse) wins, else the selected one (controller), else the last shown, else the first row
    if (g_hover.text[0]) g_shown = g_hover;
    else if (g_focus.text[0]) g_shown = g_focus;
    else if (!g_shown.text[0]) g_shown = g_first;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    float gap = 24 * g_scale;
    // the description on a dark panel framed like the rows' buttons
    skin_panel(ImGui::GetWindowDrawList(), ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + avail.x, p.y + avail.y), g_scale);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * g_scale, 20 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_shown.text);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
}

// What a row shows besides its label.
struct RowView {
    const char *value;   // right side; "" for none
    bool is_default;     // the value is the setting's default: the "default" mark, the value not highlighted
    bool arrows;         // a stepper: [<] value [>]; the mouse changes the value on the two framed arrows only
    const char *note;    // under the value instead of the "default" mark; NULL for none
    float col;           // the width of the widest value the row can show (value_width); 0: this value's
    bool end_l, end_r;   // the value is the first / last one: that arrow is drawn faint and does nothing
    bool list;           // A or a click opens a list of the values (opt_text_choice): a down arrow right of the value
};

// The width of the widest of n values at the current font size: a stepper's left arrow stands that far from the right
// one whatever value shows, so it stays under the pointer while it is clicked through the values.
static float value_width(const char *const *names, int n) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), w = 0;
    for (int i = 0; i < n; i++)
        if (names[i]) w = fmaxf(w, font->CalcTextSizeA(fs, FLT_MAX, 0, names[i]).x);
    return w;
}

// One framed arrow of a stepper over [a, b]: a white frame with the arrow in its middle, filled a little while the
// pointer is on it and more while it is pressed; faint when it does nothing (dim: the row is greyed out or the value
// is at this end).
static void arrow_box(ImDrawList *dl, ImVec2 a, ImVec2 b, bool left, bool hot, bool down, bool dim, float s) {
    const int al = dim ? 70 : hot ? 255 : 215;
    const ImU32 white = IM_COL32(255, 255, 255, al);
    if (hot && !dim) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, down ? 96 : 44), 4 * s);
    dl->AddRect(a, b, white, 4 * s, 0, fmaxf(1.0f, (hot && !dim ? 2.2f : 1.6f) * s));
    float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, aw = (b.y - a.y) * 0.38f, ah = aw * 0.8f;
    float tip = left ? -aw * 0.55f : aw * 0.55f, back = left ? aw * 0.45f : -aw * 0.45f;
    dl->AddTriangleFilled(ImVec2(cx + tip, cy), ImVec2(cx + back, cy - ah), ImVec2(cx + back, cy + ah), white);
}

// ---- test hook. NX2NAVI_MENU_TEST_CLICK=<key>:<part>[,<key>:<part>...], with NX2NAVI_MENU_TEST opening the menu on the
// page that has the rows (menu.cpp): mouse clicks on steppers by their setting's name (without NX2NAVI_), part l (the
// left arrow), r (the right arrow) or b (the row's body, on its label). One click after another, each over a dozen
// frames: the pointer goes there, the button goes down and up, and the log says what the setting is then. So a run
// checks that the arrows step a setting and that an arrow at its end and a click on the body do not, with the fonts
// and sizes of the game.
static struct ClickTest { int parsed, n, at, frame; struct { char key[56]; char part; } c[24]; } g_click;

static void click_test(const char *key, ImVec2 la, ImVec2 lb, ImVec2 ra, ImVec2 rb, ImVec2 label) {
    ClickTest &t = g_click;
    if (!t.parsed) {
        t.parsed = 1;
        const char *s = cfg_get("NX2NAVI_MENU_TEST_CLICK");
        while (s && *s && t.n < 24) {
            size_t n = strcspn(s, ",");
            const char *colon = (const char *)memchr(s, ':', n);
            if (colon) {
                snprintf(t.c[t.n].key, sizeof t.c[0].key, "NX2NAVI_%.*s", (int)(colon - s), s);
                t.c[t.n++].part = colon[1];
            }
            s += n;
            if (*s == ',') s++;
        }
    }
    if (t.at >= t.n || strcmp(key, t.c[t.at].key)) return;
    const char part = t.c[t.at].part;
    const ImVec2 to = part == 'l'   ? ImVec2((la.x + lb.x) * 0.5f, (la.y + lb.y) * 0.5f)
                      : part == 'r' ? ImVec2((ra.x + rb.x) * 0.5f, (ra.y + rb.y) * 0.5f)
                                    : label;
    ImGuiIO &io = ImGui::GetIO();
    const int f = t.frame++;
    if (f == 0) io.AddMousePosEvent(to.x, to.y);
    else if (f == 4) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    else if (f == 7) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    else if (f == 12) {
        rt_log("menu: test hook: click on %s of %s: now %s",
               part == 'l' ? "the left arrow" : part == 'r' ? "the right arrow" : "the body", key, cfg_value(key));
        t.at++, t.frame = 0;
    }
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A, or a click on a row without
// arrows), -1 / +1 (left / right, or the mouse on a stepper's framed arrow), 3 / 4 (move up / down, movable rows only)
// or 0; always 0 for a row the environment fixes (greyed out, still selectable so its description shows). movable: the
// top / left face buttons, Ctrl+Up / Ctrl+Down and two drawn arrows move it.
// A stepper takes the mouse on its two framed arrows only: a click anywhere else on the row selects it and changes
// nothing. A click on the row used to step to the next value, and the left arrow stood left of the value's text, so
// it moved when a value of another width came up and the next click at the same place landed on the row instead.
static int row_text(const char *key, const char *label, const char *desc, const RowView &v, bool movable) {
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? g_disable_note : NULL;
    g_disable_next = false;
    bool off = disabled || cfg_from_env(key) != 0;
    const bool lit = g_lit_next;
    g_lit_next = false;
    ImGui::PushID(key);
    // the row's box, inset from the pane's sides by the room its cursor needs
    const float margin = row_margin();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // h whole pixels: ImGui truncates cursor positions, which rounds a fractional height the other way above the window's
    // top (negative y), so the page grew a pixel per row scrolled out and a row scrolled to the bottom was cut off
    // at least a pixel wide: InvisibleButton asserts on a zero size (a narrow window), and ImGui reads a negative one as
    // "up to that far before the right edge", away from the box drawn
    float fs = ImGui::GetFontSize(), w = fmaxf(ImGui::GetContentRegionAvail().x - margin, 1.0f), h = floorf(fs * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the row's own border shows the selection
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool to_key = g_focus_key[0] && !strcmp(key, g_focus_key);
    if (g_take_first || to_key) {
        take_cursor(ImGui::GetItemID());
        if (g_take_first) ImGui::SetScrollY(0);   // the page's top: text and a heading above the first row show
        else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);   // as a cursor move would (the last row whole)
        g_take_first = false;
        if (to_key) g_focus_key[0] = 0;
    }
    if (g_resume) {   // opt_focus_resume: the kept row takes the cursor if it is wholly in view, so the scroll stays
        const ImRect &clip = ImGui::GetCurrentWindow()->InnerClipRect;
        const bool whole = ImGui::GetItemRectMin().y >= clip.Min.y && ImGui::GetItemRectMax().y <= clip.Max.y;
        if (whole && !g_resume_fallback) g_resume_fallback = ImGui::GetItemID();
        if (whole && !strcmp(key, g_resume_key)) {
            take_cursor(ImGui::GetItemID());
            g_resume = false;
        }
    }
    if (ImGui::IsItemFocused()) {   // the page's row with the cursor, for opt_focus_resume
        Place *pl = place_of(g_page, true);
        snprintf(pl->key, sizeof pl->key, "%s", key);
    }
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    // a click of the mouse, not A / Enter / Space: only a click picks an arrow by where the pointer is
    const bool by_mouse = clicked && hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    g_held = false;
    if (foc && !off) {
        // the left stick arrives as the D-pad (menu.cpp pad_events)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        static const ImGuiKey steps[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                         ImGuiKey_RightArrow};
        for (ImGuiKey k : steps) {
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
            // left and right belong to the row (navigation would otherwise move sideways into the description pane)
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
        if (movable) {
            bool ctrl = ImGui::GetIO().KeyCtrl;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                step = 3;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
                step = 4;
        }
    }
    if (foc) desc_set(g_focus, desc, key, m, disabled);
    if (hov) desc_set(g_hover, desc, key, m, disabled);
    if (!g_first.text[0]) desc_set(g_first, desc, key, m, disabled);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float pad = 20 * g_scale, small = fs * 0.72f;
    // the save menu's brown button, brighter while selected or hovered; the white cursor goes round it in opt_end
    skin_button(dl, p, ImVec2(p.x + w, p.y + h), g_scale, foc || lit ? 1.0f : hov ? 0.6f : 0.0f, off);
    if (foc || lit || (hov && !g_cursor.on)) {
        g_cursor.on = true, g_cursor.a = p, g_cursor.b = ImVec2(p.x + w, p.y + h);
        g_cursor.alpha = (foc || lit ? 1.0f : 0.55f) * (off ? 0.5f : 1.0f);
    }
    float ty = p.y + fs * 0.25f, ny = p.y + fs * 1.3f;   // the label line and the note line under it
    float right = p.x + w - pad;                         // the value's right edge
    float value_left = right;
    // The value and its note take at most 60 % of the row: a longer value (a graphics card's full name) is drawn smaller,
    // down to 75 % of the font size, then clipped, so the label keeps room and nothing reaches past the box.
    const float cap = fmaxf((w - pad * 2) * 0.6f, 0.0f);
    float vfs = fs, tv = v.value[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0, v.value).x : 0;
    if (tv > cap) {
        vfs = fmaxf(fs * cap / tv, fs * 0.75f);
        tv = fminf(font->CalcTextSizeA(vfs, FLT_MAX, 0, v.value).x, cap);
    }
    float value_mid = -1;   // a stepper's value and its note are centred here, between the arrows
    if (v.arrows) {         // [<] value [>]: the arrows are drawn (the font has no arrow glyphs), each in a white frame
        // The frames stand in fixed places: the right one at the row's right edge, the left one the width of the row's
        // widest value before it. They fit over the note line, in the label line's height.
        const float bw = floorf(fs * 1.3f), bh = floorf(fs * 1.02f), gap = floorf(fs * 0.4f), cy = floorf(ty + fs * 0.52f);
        float col = fminf(fmaxf(v.col, tv), fmaxf(cap - 2 * (bw + gap), fs));
        if (tv > col) {   // a value wider than the room between the arrows: smaller, then clipped
            vfs = fmaxf(fs * col / font->CalcTextSizeA(fs, FLT_MAX, 0, v.value).x, fs * 0.75f);
            tv = fminf(font->CalcTextSizeA(vfs, FLT_MAX, 0, v.value).x, col);
        }
        const ImVec2 ra(right - bw, cy - bh * 0.5f), rb(right, cy + bh * 0.5f);
        const ImVec2 la(ra.x - gap - col - gap - bw, ra.y), lb(ra.x - gap - col - gap, rb.y);
        if (!g_click.parsed || g_click.at < g_click.n) click_test(key, la, lb, ra, rb, ImVec2(p.x + w * 0.25f, p.y + h * 0.5f));
        const ImVec2 mp = ImGui::GetIO().MousePos;
        // the pointer on an arrow: its frame and two pixels around it, so a click on the frame's own line counts
        const float slop = 2 * g_scale;
        auto on = [&](ImVec2 a, ImVec2 b) {
            return hov && mp.x >= a.x - slop && mp.x < b.x + slop && mp.y >= a.y - slop && mp.y < b.y + slop;
        };
        const bool hl = on(la, lb), hr = on(ra, rb), down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (by_mouse) step = 0;   // a click on the row itself only selects it
        // The press steps, and a held button repeats as a held key does; after a second it also goes ten steps at a
        // time over a long range (opt_slider), as held left / right do.
        if (!off && ((hl && !v.end_l) || (hr && !v.end_r)) && ImGui::IsMouseClicked(ImGuiMouseButton_Left, true)) {
            step = hl ? -1 : 1;
            if (ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left] > 1.0f) g_held = true;
        }
        arrow_box(dl, la, lb, true, hl, hl && down, off || v.end_l, g_scale);
        arrow_box(dl, ra, rb, false, hr, hr && down, off || v.end_r, g_scale);
        value_mid = (lb.x + ra.x) * 0.5f;
        right = ra.x - gap;
        value_left = la.x;
    }
    if (v.list) {   // a down arrow at the row's right edge, the value left of it: the row opens a list
        const float aw = floorf(fs * 0.5f), ah = aw * 0.6f, ax = right - aw, cy = ty + fs * 0.52f;
        const ImU32 ac = off ? kGrey : hov || foc || lit ? SKIN_TEXT : SKIN_TEXT_DIM;
        dl->AddTriangleFilled(ImVec2(ax, cy - ah * 0.5f), ImVec2(ax + aw, cy - ah * 0.5f), ImVec2(ax + aw * 0.5f, cy + ah * 0.5f), ac);
        right = ax - floorf(fs * 0.5f);
    }
    if (movable) {   // up and down arrows left of the value; a click on one moves the row
        // a fixed value column (On / Off differ in width) keeps the arrows of the rows in one line
        float aw = fs * 0.42f, gap = fs * 0.55f;
        float cx = right - fmaxf(tv, fs * 2.8f) - gap * 2 - aw;
        float uy = ty + fs * 0.12f, dy = ty + fs * 0.92f;
        ImU32 ac = off ? kGrey : hov || foc ? SKIN_TEXT : SKIN_TEXT_DIM;
        dl->AddTriangleFilled(ImVec2(cx, uy + aw * 0.7f), ImVec2(cx + aw, uy + aw * 0.7f), ImVec2(cx + aw * 0.5f, uy), ac);
        dl->AddTriangleFilled(ImVec2(cx, dy - aw * 0.7f), ImVec2(cx + aw, dy - aw * 0.7f), ImVec2(cx + aw * 0.5f, dy), ac);
        if (by_mouse && !off) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= cx - gap * 0.5f && mp.x <= cx + aw + gap * 0.5f) step = mp.y < ty + fs * 0.52f ? 3 : 4;
        }
        value_left = cx - gap;
    }
    if (v.value[0]) {
        float vx = value_mid >= 0 ? floorf(value_mid - tv * 0.5f) : right - tv;
        ImVec4 vclip(vx, p.y, vx + tv, p.y + h);
        dl->AddText(font, vfs, ImVec2(vx, ty + (fs - vfs) * 0.5f),
                    off ? kGrey : v.is_default ? IM_COL32(232, 222, 198, 255) : IM_COL32(255, 255, 255, 255), v.value, NULL,
                    0, &vclip);
        if (!v.arrows && !movable) value_left = vx;
    }
    float note_right = p.x + w - pad;   // where the label's own note stops: short of the value's note, when there is one
    if (v.note || v.is_default) {   // under the value, right-aligned with it; its start shows when it is too long
        const char *t = v.note ? v.note : menu_text(UI_MENU_DEFAULT_MARK);
        float tw = fminf(font->CalcTextSizeA(small, FLT_MAX, 0, t).x, cap);
        // a stepper's note is centred under its value, as far as the row's right edge lets it
        float nx = value_mid >= 0 ? fminf(floorf(value_mid - tw * 0.5f), p.x + w - pad - tw) : right - tw;
        ImVec4 nclip(nx, p.y, nx + tw, p.y + h);
        dl->AddText(font, small, ImVec2(nx, ny), v.note ? IM_COL32(250, 206, 130, 255) : SKIN_TEXT_DIM, t, NULL, 0, &nclip);
        if (tw > 0) note_right = nx - fs * 0.6f;
    }
    // the label and its note, clipped before the value. A restart-only setting says "after restart" while its value
    // differs from the one the game started with (cfg_pending), so the note marks what a restart would change; the
    // description says it whatever the value (desc_set)
    ImVec4 clip(p.x, p.y, value_left - fs * 0.6f, p.y + h);
    dl->AddText(font, fs, ImVec2(p.x + pad, ty), off ? kGrey : SKIN_TEXT, label, NULL, 0, &clip);
    const char *note = disabled                          ? disabled
                     : off                               ? menu_text(UI_MENU_SET_BY_ENV)
                     : m && !m->live && cfg_pending(key) ? menu_text(UI_MENU_AFTER_RESTART)
                                                         : NULL;
    ImVec4 note_clip(p.x, p.y, note_right, p.y + h);
    if (note)
        dl->AddText(font, small, ImVec2(p.x + pad, ny), off ? IM_COL32(200, 150, 90, 255) : SKIN_TEXT_DIM, note, NULL, 0,
                    &note_clip);
    ImGui::PopID();
    return off ? 0 : step;
}

static int row(const char *key, UiTextId label, UiTextId desc, const RowView &v) {
    return row_text(key, menu_text(label), menu_text(desc), v, false);
}

int opt_text_row(const char *id, const char *label, const char *value, const char *note, const char *desc,
                 bool movable) {
    RowView v = {value ? value : "", false, false, note};
    return row_text(id, label, desc, v, movable);
}

// ---- writing a setting: at once in memory (live settings follow), to nx2navi.ini a moment later (opt_save_tick)
static uint64_t g_save_at;   // plat_now_ns deadline of the pending write; 0 none
static bool g_save_logged;

static void apply(const char *key, const char *value) {
    if (*value) {
        cfg_set(key, value);
    } else {   // the automatic choice: the key unset, its line removed from nx2navi.ini
        const char *k = key;
        cfg_reset(&k, 1);
    }
    opt_save_soon();
    menu_sound_play(MENU_SOUND_CURSOR);
}

void opt_save_soon(void) { g_save_at = plat_now_ns() + 400000000ull; }

int opt_save_tick(int force) {
    if (!g_save_at || (!force && plat_now_ns() < g_save_at)) return g_save_at != 0;
    g_save_at = 0;
    if (!cfg_save() && !g_save_logged) {
        g_save_logged = true;
        rt_log("menu: could not write nx2navi.ini; the changed settings hold for this session");
    }
    return 0;
}

// ---- rows

// An on / off row is a stepper too: either arrow, A, left or right flips it.
static float on_off_width(void) {
    const char *const names[] = {menu_text(UI_MENU_ON), menu_text(UI_MENU_OFF)};
    return value_width(names, 2);
}

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted) {
    const char *d = cfg_default(key);
    bool set = cfg_bool(key) != 0, def = d && *d && strcmp(d, "0") != 0;
    bool on = set != inverted;   // what the row shows
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), set == def, true, NULL, on_off_width()};
    if (!row(key, label, desc, v)) return false;   // any step flips it
    apply(key, set ? "0" : "1");
    return true;
}

bool opt_toggle_value(const char *key, UiTextId label, UiTextId desc, const char *on_value, const char *off_value, const char *note) {
    // the default as the player's profile has it (cfg_default: the Steam Deck's fifo), which is also what "on" writes
    // when it is an on value, so turning the switch off and on again puts the profile's own value back
    const char *d = cfg_default(key);
    bool on = strcmp(cfg_value(key), off_value) != 0, def = d && strcmp(d, off_value) != 0;
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), on == def, true, note, on_off_width()};
    if (!row(key, label, desc, v)) return false;   // any step flips it
    apply(key, on ? off_value : def && *d ? d : on_value);
    return true;
}

bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value) {
    RowView v = {value ? value : "", false, false, NULL};
    return row(id, label, desc, v) == 2;   // A or a click; left and right do nothing
}

bool opt_toggle_text(const char *id, const char *label, const char *desc, bool on, const char *value) {
    // off is the plain state: its value drawn dim, without the "default" mark
    RowView v = {value ? value : menu_text(on ? UI_MENU_ON : UI_MENU_OFF), !on, true, on ? NULL : "",
                 value ? 0 : on_off_width()};
    return row_text(id, label, desc, v, false) != 0;   // any step flips it
}

// A group heading: the game's divider ornament across the pane, then the title in the accent colour.
static void group_divider(void) {
    float fh = skin_filigree_height(g_scale);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float m = row_margin();
    skin_filigree(ImGui::GetWindowDrawList(), p.x + m, p.x + ImGui::GetContentRegionAvail().x - m, p.y + fh * 0.5f, g_scale);
    ImGui::Dummy(ImVec2(0, fh));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m);
}

// Text across the rows' column, inset from its sides as the rows' boxes are and wrapped before their right edge. The
// cursor already stands at the inset when indented is true (after group_divider).
static void column_text(const char *text, const ImVec4 *col, bool indented) {
    const float m = row_margin();
    if (!indented) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fmaxf(ImGui::GetContentRegionAvail().x - m, 1.0f));
    if (col) ImGui::TextColored(*col, "%s", text);
    else ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

static const ImVec4 kWarn(0.95f, 0.66f, 0.40f, 1);

void opt_group_text(const char *title) {
    group_divider();
    const ImVec4 gold = ImGui::ColorConvertU32ToFloat4(SKIN_GOLD);
    column_text(title, &gold, true);
}

void opt_text_str(const char *text, bool warn) { column_text(text, warn ? &kWarn : nullptr, false); }

void opt_group(UiTextId title) {
    group_divider();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(SKIN_GOLD), "%s", menu_text(title));
}

void opt_text(UiTextId text, bool warn) { column_text(menu_text(text), warn ? &kWarn : nullptr, false); }

bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur, const char *note) {
    const char *d = cfg_default(key);
    const char *now = cfg_value(key);
    if (cur < 0)
        for (int i = 0; i < n; i++)
            if (!strcmp(values[i], now)) cur = i;
    int def = -1;
    for (int i = 0; d && i < n; i++)
        if (!strcmp(values[i], d)) def = i;
    RowView v = {cur >= 0 ? names[cur] : now, cur == def, true, note, value_width(names, n), cur == 0, cur == n - 1};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return false;
    int nv;
    if (cur < 0) nv = step < 0 ? n - 1 : 0;   // a value not in the list: left from the end, right / A from the start
    else if (step == 2) nv = (cur + 1) % n;               // A / click: the next value, round to the first
    else nv = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;   // left / right stop at the ends
    if (nv == cur) return false;
    apply(key, values[nv]);
    return true;
}

int opt_choice_index(const char *key, UiTextId label, const char *const *names, int n, int cur, int def, UiTextId desc,
                     const char *note) {
    RowView v = {cur >= 0 && cur < n ? names[cur] : "", cur == def, true, note, value_width(names, n), cur == 0,
                 cur == n - 1};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return -1;
    int nv = cur < 0 ? (step < 0 ? n - 1 : 0) : step == 2 ? (cur + 1) % n : cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
    if (nv == cur) return -1;
    menu_sound_play(MENU_SOUND_CURSOR);
    return nv;
}

void opt_apply(const char *key, const char *value) {   // as apply, without its sound: a row writing several keys
    if (*value) {
        cfg_set(key, value);
    } else {
        const char *k = key;
        cfg_reset(&k, 1);
    }
    opt_save_soon();
}

bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc, const char *note) {
    enum { MAX = 32 };
    const CfgMeta *m = cfg_meta(key);
    const char *values[MAX], *shown[MAX];
    char buf[256];
    int k = 0;
    if (m && !m->def[0]) values[k++] = "";   // the automatic choice
    snprintf(buf, sizeof buf, "%s", m && m->values ? m->values : "");
    for (char *s = buf, *e; *s && k < MAX; s = e) {
        e = strchr(s, '|');
        if (e) *e++ = 0;
        else e = s + strlen(s);
        values[k++] = s;
    }
    if (n > k) n = k;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    return opt_choice_list(key, label, values, shown, n, desc, -1, note);
}

bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step, const char *fmt,
                const char *note, const char *fallback) {
    const char *set = cfg_get(key);
    const char *src = fallback && !(set && *set) ? fallback : key;   // unset: show the value in effect, the fallback's
    const char *d = cfg_default(src);
    // A key without a default of its own (the per-stick dead zones) defaults to its fallback: the "default" mark
    // follows the fallback's default, not 0, even once the key is set.
    if (fallback && !(d && *d)) d = cfg_default(fallback);
    double v = cfg_double(src), def = d && *d ? strtod(d, NULL) : lo;
    char shown[48], ends[2][48];
    snprintf(shown, sizeof shown, fmt, v);
    snprintf(ends[0], sizeof ends[0], fmt, (double)lo);
    snprintf(ends[1], sizeof ends[1], fmt, (double)hi);
    const char *const widest[] = {shown, ends[0], ends[1]};
    RowView rv = {shown, fabs(v - def) < step * 0.01, true, note, value_width(widest, 3), v <= lo + step * 0.01,
                  v >= hi - step * 0.01};
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int steps = g_held && (hi - lo) > 40 * step ? 10 : 1;   // held: ten steps at a time over a long range
    double n = round(v / step) * step + (s == 2 ? step : s * step * steps);
    if (s == 2 && n > hi + step * 0.01) n = lo;   // A / click past the top goes round; left / right stop at the ends
    n = n < lo ? lo : n > hi ? hi : n;
    if (fabs(n - v) < step * 0.01) return false;
    char out[32];
    if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(n));
    else snprintf(out, sizeof out, "%g", round(n / step) * step);
    // Landing on the fallback's value unsets the key (its line leaves nx2navi.ini), so the row follows the fallback again.
    apply(key, fallback && fabs(n - cfg_double(fallback)) < step * 0.01 ? "" : out);
    return true;
}

// ---- binding rows (the remapping rows of the Controls tab): a label and three cells, each its own nav item

// Text centred (left-aligned when centred is false) in [x0, x1] on a row from y to y + h, at the font size or smaller so
// a long name ("Нажатие правого стика", "Стрелка вправо") fits; under 65 % it is clipped instead.
static void fit_text(ImDrawList *dl, float x0, float x1, float y, float h, ImU32 col, const char *t, bool centred) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), room = x1 - x0, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    if (tw > room) {
        fs = fmaxf(fs * room / tw, fs * 0.65f);
        tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    }
    float x = centred && tw < room ? x0 + (room - tw) * 0.5f : x0;
    ImVec4 clip(x0, y, x1, y + h);
    dl->AddText(font, fs, ImVec2(x, y + (h - fs) * 0.5f), col, t, NULL, 0, &clip);
}

// The label column and the three cells of a binding row of width w.
static void bind_columns(float x, float w, float cx[3], float *cw) {
    float lw = floorf(w * 0.34f), gap = floorf(8 * g_scale);
    *cw = fmaxf(floorf((w - lw - gap * 3) / 3), 1.0f);   // a pixel at least: InvisibleButton asserts on a zero width
    for (int c = 0; c < 3; c++) cx[c] = x + lw + gap + c * (*cw + gap);
}

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2) {
    // over the cells of the binding rows: the same inset (opt_bind_row) and the same columns
    const float margin = row_margin();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = fmaxf(ImGui::GetContentRegionAvail().x - margin * 2, 1.0f), cx[3], cw, fs = ImGui::GetFontSize() * 0.8f;
    bind_columns(p.x + margin, w, cx, &cw);
    const UiTextId cols[3] = {c0, c1, c2};
    ImFont *font = ImGui::GetFont();
    for (int c = 0; c < 3; c++) {
        const char *t = menu_text(cols[c]);
        float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(cx[c] + (cw - tw) * 0.5f, p.y), SKIN_TEXT_DIM, t);
    }
    ImGui::Dummy(ImVec2(w, fs * 1.2f));
}

int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note) {
    return opt_bind_row(key, label, menu_text(desc), cells, enabled, note);
}

int opt_bind_row(const char *key, const char *label, const char *desc, const char *const cells[3], const bool enabled[3],
                 const char *note) {
    ImGui::PushID(key);
    const float margin = row_margin();   // room for the cursor, as the option rows leave
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x - margin, h = floorf(fs * 1.9f), cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 6 * g_scale;
    fit_text(dl, p.x + 16 * g_scale, cx[0] - fs * 0.3f, p.y, h, IM_COL32(240, 236, 224, 255), label, false);
    int result = -1;
    bool row_foc = false, row_hov = false;
    for (int c = 0; c < 3; c++) {
        ImVec2 a(cx[c], p.y), b(cx[c] + cw, p.y + h);
        const char *t = cells[c] && cells[c][0] ? cells[c] : "-";
        if (!enabled[c]) {   // not a nav item: the cursor skips it
            skin_button(dl, a, b, g_scale, 0, true);
            fit_text(dl, a.x + pad, b.x - pad, a.y, h, kGrey, t, true);
            continue;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the cell's own border shows the selection
        bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, h), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        char id[80];
        snprintf(id, sizeof id, "%s#%d", key, c);
        bool to_key = g_focus_key[0] && !strcmp(id, g_focus_key);
        if (g_take_first || to_key) {
            take_cursor(ImGui::GetItemID());
            if (g_take_first) ImGui::SetScrollY(0);
            else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            g_take_first = false;
            if (to_key) g_focus_key[0] = 0;
        }
        bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        row_foc |= foc;
        row_hov |= hov;
        if (clicked) result = c;
        // Clearing: Delete, Backspace or X on the cell the cursor is on, or, used with the mouse (no cursor shown), on
        // the cell under the pointer; a right-click clears the cell under the pointer at any time. A click puts the
        // cursor away, so without the pointer cases a mouse user could not clear a cell at all: Delete pressed after
        // the click went to the capture window and became the new binding.
        bool clear_key = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
        bool mouse_mode = !ImGui::GetCurrentContext()->NavCursorVisible;
        if ((foc || (hov && mouse_mode)) && clear_key) result = 10 + c;
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) result = 10 + c;
        skin_button(dl, a, b, g_scale, foc ? 1.0f : hov ? 0.6f : 0.0f, false);   // a small brown button per cell
        if (foc || (hov && !g_cursor.on)) g_cursor.on = true, g_cursor.a = a, g_cursor.b = b, g_cursor.alpha = foc ? 1.0f : 0.55f;
        fit_text(dl, a.x + pad, b.x - pad, a.y, h, SKIN_TEXT, t, true);
        ImGui::PopID();
    }
    char k[64];
    snprintf(k, sizeof k, "NX2NAVI_%s", key);
    if (row_foc) desc_set(g_focus, desc, k, NULL, NULL);
    if (row_hov) desc_set(g_hover, desc, k, NULL, NULL);
    if (!g_first.text[0]) desc_set(g_first, desc, k, NULL, NULL);
    if (note && (row_foc || row_hov)) {   // e.g. what moved here, after the description
        Desc &d = row_hov ? g_hover : g_focus;
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", note);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return result;
}

// ---- a choice with a list (opt_text_choice): the list is a popup under the row (above it when the screen ends first),
// as wide as the row and at most kListRows values high, scrolled to the value in effect

static const int kListRows = 12;
static int g_list_frame = -10;   // the last frame a list was open
static ImGuiID g_list_id;        // that list

bool opt_list_open(void) { return g_list_frame >= ImGui::GetFrameCount() - 1; }

static bool back_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

int opt_text_choice(const char *id, const char *label, const char *const *names, int n, int cur, const char *desc,
                    bool wrap, const char *shown, const char *note) {
    ImGui::PushID(id);
    const ImGuiID list = ImGui::GetID("##list");
    ImGui::PopID();
    g_lit_next = ImGui::IsPopupOpen(list, 0);
    RowView v = {shown ? shown : cur >= 0 && cur < n ? names[cur] : "", false, false, note};
    v.list = n > 0;
    const int step = row_text(id, label, desc, v, false);
    const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    int pick = -1;
    if (n > 0 && (step == -1 || step == 1)) {
        if (cur < 0) pick = step < 0 ? n - 1 : 0;   // no value of the list in effect: from its end or its start
        else if (wrap) pick = (cur + step + n) % n;
        else pick = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
    }
    ImGui::PushID(id);
    if (step == 2 && n > 0) ImGui::OpenPopup("##list");
    const int frame = ImGui::GetFrameCount();
    // B or Escape closed the list before the rows were drawn (ImGui's own cancel): the cancel sound the menu's boxes play
    if (!ImGui::IsPopupOpen(list, 0) && g_list_id == list && g_list_frame == frame - 1 && back_pressed())
        menu_sound_play(MENU_SOUND_CANCEL);
    if (ImGui::IsPopupOpen(list, 0)) {
        const float fs = ImGui::GetFontSize(), ih = floorf(fs * 1.7f), pad = floorf(10 * g_scale), w = rb.x - ra.x;
        const float lh = ih * (n < kListRows ? n : kListRows) + pad * 2, gap = 4 * g_scale;
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float top = vp->WorkPos.y, bottom = vp->WorkPos.y + vp->WorkSize.y;
        float y = rb.y + gap;
        if (y + lh > bottom) y = ra.y - gap - lh >= top ? ra.y - gap - lh : fmaxf(bottom - lh, top);
        ImGui::SetNextWindowPos(ImVec2(ra.x, y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, lh));   // the popup fits its values up to that
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));   // skin_panel draws the box
        const bool open = ImGui::BeginPopup("##list", ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (open) {
            g_list_frame = frame, g_list_id = list;
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            skin_panel(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), g_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.22f, 0.12f, 1));          // the value in effect
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.42f, 0.31f, 0.15f, 1));   // under the pointer
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.50f, 0.37f, 0.18f, 1));
            for (int i = 0; i < n; i++) {
                ImGui::PushID(i);
                // the name is drawn by fit_text, not as the selectable's label: a name may hold "##"
                if (ImGui::Selectable("", i == cur, 0, ImVec2(0, ih))) pick = i;   // and the list closes
                if (i == cur && ImGui::IsWindowAppearing()) {
                    ImGui::SetItemDefaultFocus();   // the controller's cursor starts on the value in effect
                    ImGui::SetScrollHereY(0.5f);
                }
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                fit_text(dl, a.x + pad, b.x - pad, a.y, b.y - a.y, i == cur ? SKIN_GOLD : SKIN_TEXT,
                         names[i] ? names[i] : "", false);
                ImGui::PopID();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (pick < 0 && back_pressed()) {
                ImGui::CloseCurrentPopup();
                menu_sound_play(MENU_SOUND_CANCEL);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    return pick == cur ? -1 : pick;
}
