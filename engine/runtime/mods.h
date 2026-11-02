// mods.h: the player's mods (mods.cpp). The launchers name the mods folders in TOTK_MODS_DIR (';' on Windows, ':'
// elsewhere; the Windows launcher: <its folder>\mods, then %APPDATA%\nxrecomp\TotK-engine\mods); each folder in them
// is one mod, laid out as emulators and the Switch take them (mods_scan.h); a name starting with '_' or '.' is left
// out. The list is scanned once, at the first ask; its order (top wins a file two mods replace) and switches are the
// settings keys mods.order (names top first, '|' between them) and mods.off (names switched off): new mods come on,
// below the known ones by name. hle_fs serves the RomFS of the mods on and usable at start; the F10 Mods tab
// (pc_mods_ui.cpp) edits order and switches for the next start.
// TOTK_USER_MODS (a list of RomFS folders, the Linux launcher's own Mods page and tests) replaces the scan: those
// folders, in that order, all on, shown read-only.
#pragma once
#include <string>
#include <vector>

enum nxr_mod_problem {
    NXR_MOD_OK,
    NXR_MOD_EMPTY,        // no RomFS file, no ExeFS patch, no cheat file
    NXR_MOD_BAD_NAME,     // its folder name holds '|', the separator of the order keys
    NXR_MOD_OTHER_GAME,   // its contents/<id>/ is another title's
    NXR_MOD_DUPLICATE,    // a mod of the same name is in an earlier mods folder
    NXR_MOD_PLUGIN,       // code modules (exefs/subsdk*, a replaced main): no module loader, the whole mod stays off
    NXR_MOD_EXEFS_ONLY,   // only ExeFS patches / cheats and no mods_exefs_hook to use them
    NXR_MOD_EXEFS,        // the hook found its ExeFS side unusable for the whole mod: problem_note says why
};

struct nxr_mod {
    std::string name;            // folder name: the key in mods.order / mods.off
    std::string dir;             // the folder (UTF-8)
    std::string romfs;           // its RomFS root ("" none)
    std::string exefs, cheats;   // its ExeFS and cheats folders ("" none)
    std::string layout;          // where its RomFS was found ("romfs", "atmosphere/contents/<id>/romfs", "folder", ...)
    std::string collection;      // a mod of a copied emulator collection: its folder below the mods folder
                                 // ("yuzu/load/0100F2C0115B6000/<mod>"), shown in its F10 row; "" a mod of its own
    std::string other_title;     // NXR_MOD_OTHER_GAME: the title id it is for
    int kind = 0;                // mods_scan::Kind bits
    bool on = true;              // the player's switch
    nxr_mod_problem problem = NXR_MOD_OK;
    // hle_fs (mods_count_files), once the game opened its first RomFS file; counted = false until then
    bool counted = false;
    int replaced = 0, added = 0;          // files it serves (replacing the game's / new)
    int by_port = 0;                      // files the port's own serve instead (glyph bank, fixes, our sets)
    int by_mod = 0;                       // files a higher mod serves instead
    int other_version = 0;                // files for another game version (.Product.<n>.), not used
    std::string by_mod_name;              // the first higher mod that took one of its files
    // mods_exefs_hook fills these (mods-exefs)
    int exefs_patches = 0;                // ExeFS patch files for this game's build
    int exefs_other = 0;                  // ExeFS patch files for other builds
    int cheats_count = 0;                 // cheat files / entries
    std::string exefs_note;               // one line under the row, already in the player's language ("" none)
    std::string problem_note;             // NXR_MOD_EXEFS: why, already in the player's language
};

// mods-exefs (mods_exefs.cpp sets it from a static initializer; null: no ExeFS side): called with the whole list (top
// first; duplicates and '|' names included, with their problem set) at the end of each scan, before the RomFS list of
// the run is taken, so setting a mod's `problem` keeps its RomFS out too. It runs under the list's lock: it must not
// call the mods_* / nxr_mods_* functions, nor reorder, add or remove entries.
extern void (*mods_exefs_hook)(std::vector<nxr_mod> &list);
// A change of another kind (a cheat, a patch set) that applies at the next start: the F10 notice shows (`why`: log)
void mods_need_restart(const char *why);

// The list as it is now (the tab's edits included), top first; scans at the first call. Any thread.
std::vector<nxr_mod> mods_list();
// The RomFS folders the game serves this run (mods on and usable at the first call), top first
const std::vector<std::string>& mods_romfs_start();
// The first mods folder (the tab's "Open mods folder"), "" none
std::string mods_folder();
bool mods_read_only();   // TOTK_USER_MODS: the launcher chose the list
// The tab's edits (present thread): saved at once, used at the next start
void mods_set_on(int i, bool on);
void mods_move(int i, int delta);   // -1 up, +1 down
bool mods_changed();                // order or switches differ from this run's, or mods_need_restart was called
// ---- for the ExeFS patches and cheats (mods-exefs): the mods as they were at start, top first, fixed for the run (the
// strings live as long as the process). Any thread.
extern "C" {
int nxr_mods_count(void);
const char *nxr_mods_name(int i);     // the folder name (the key in mods.order / mods.off)
const char *nxr_mods_dir(int i);      // the mod folder (UTF-8)
const char *nxr_mods_exefs(int i);    // its exefs folder in the emulator / Atmosphere layouts ("" none)
const char *nxr_mods_cheats(int i);   // its cheats folder ("" none)
int nxr_mods_on(int i);               // the player switched it on and it is usable (problem NXR_MOD_OK)
}

// hle_fs: the counts of mod `romfs` (a root of mods_romfs_start)
void mods_set_counts(const std::string& romfs, int replaced, int added, int by_port, int by_mod, int other_version,
                     const std::string& by_mod_name);
