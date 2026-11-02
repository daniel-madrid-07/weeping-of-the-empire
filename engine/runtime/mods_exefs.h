// mods_exefs.h: the mods' ExeFS patches and Atmosphere cheats in the run (mods_exefs.cpp over exefs_scan.h and mods.h).
#pragma once
#include <string>
#include <vector>

// rt_main, right after main's image is copied in and before it is relocated (img_dir: the image folder, TOTK_IMG).
// Scans the mods (mods_list), writes the patches this build can use into main's memory, starts the rebuild when the
// mods' code patches differ from the ones this exe was built with (and exits), loads the enabled cheats.
// No mods with ExeFS files or cheats: nothing happens.
void mods_exefs_start(const char *img_dir);

// The F10 Cheats tab (pc_cheats_ui.cpp): the cheats of the mods on, in mod order. Present thread.
struct nxr_cheat {
    std::string mod, name;
    std::string note;   // why it cannot be used, or that it needs the next start ("" none); in the player's language
    bool on = false;    // the player's switch (saved: cheats.on)
    bool usable = true;
    bool code = false;  // it changes the game's code: built in at the next start
};
std::vector<nxr_cheat> mods_cheats();
void mods_cheat_set(int i, bool on);   // saved at once; a data cheat runs (or stops) right away
