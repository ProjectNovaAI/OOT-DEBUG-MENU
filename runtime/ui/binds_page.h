// The remapping rows of the menu's Controls tab (ui/binds_page.cpp). Present path only (ImGui).
#pragma once
#include <stddef.h>

// Inside the page's opt_begin / opt_end: every action's two keys and its controller button, remapped by selecting a
// cell and pressing the new one, and the reset of them all. binds_after_page, after opt_end: the window that waits for
// the press.
void binds_rows(float scale);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);
// The menu opened or closed (menu.cpp): a cell still waiting for a press stops waiting, unchanged. Its window is closed
// with the menu's popups.
void binds_cancel(void);
// A key or mouse button (KEY_ id) and a controller button (PAD_ id) as the cells show them; "" for none.
const char *binds_key_label(int k, char *buf, size_t n);
const char *binds_pad_label(int p);
