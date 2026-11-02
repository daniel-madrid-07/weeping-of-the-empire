// pc_cheats_ui.h: the F10 window's "Cheats" tab (pc_overlay.cpp) over mods_exefs.h: the Atmosphere cheats of the mods on,
// grouped by mod, a switch each; the ones the port cannot run greyed with why. A data cheat runs or stops at once; one that
// changes the game's code is built in at the next start.
#pragma once

bool pc_cheats_tab_available();   // a mod on has a cheat file for this game version
void pc_cheats_tab();             // the tab's contents (present thread, inside the tab item)
