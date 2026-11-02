// pc_mods_ui.h: the F10 window's "Mods" tab (pc_overlay.cpp) over mods.h: the player's mods in load order, each with
// its switch, up / down, what it serves and why one is not used; "Open mods folder". Edits apply at the next start.
#pragma once

bool pc_mods_tab_available();   // a mods folder is in use (the launchers set TOTK_MODS_DIR / TOTK_USER_MODS)
void pc_mods_tab();             // the tab's contents (present thread, inside the tab item)
bool pc_mods_changed();         // order or switches differ from this run's (the "apply at the next start" notice)
