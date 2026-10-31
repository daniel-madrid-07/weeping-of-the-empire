// pc_bind_ui.h: the rebinding pages of pc_bind_pages.inc for the two front ends: the F10 window's "Keys" tab
// (pc_overlay.cpp) and the key rows of the game's Options screen (totk_options_menu.cpp). Both call the pc_bind_*
// (pc_input.h) with an entry's scope and action.
#pragma once

struct PcPageEntry {
    const char *page;    // the F10 section ("Movement", "Combat" ...)
    int scope;           // PC_SCOPE_* for ROOT / GAMEPLAY / UI / MENUS, else the context's PcCtx
    const char *action;  // the row's action in pc_scheme.inc / pc_scheme_gameplay.inc
    const char *label;
};

// Every PC_PAGE line, in the file's order
const PcPageEntry *pc_bind_page_entries(int *count);
// The first entry for an action (its scope for a front end that names actions only), null if the pages have none
const PcPageEntry *pc_bind_page_find(const char *action);

// The tab is offered: the rebinding API answers (the stubs return PCB_NOT_READY), or TOTK_OPTIONS_KEYS is set to
// anything but 0 (a boot that checks the tab itself; the same switch as the Options key rows)
bool pc_bind_keys_available();
// The "Keys" tab's contents (present thread, inside the tab item): one row per entry, the pages as sections
void pc_bind_keys_tab();
// The tab is not drawn this frame (another tab, or the window closed): a capture it started ends
void pc_bind_keys_closed();
