/* Save import and backup for the Windows launcher. Pure file operations, no UI: launcher.c asks the questions,
 * test_saves.c checks these against fake folder trees.
 *
 * <base> is the player's data folder (%APPDATA%\nxrecomp\TotK-engine): the game's save is <base>\userdata\save (the
 * game reads TOTK_USERDATA\save, runtime/hle_fs.cpp, with no user-id level), the backups are <base>\backups\
 * save-YYYYMMDD-HHMMSS-NN-<why>\ (NN: the backups of the same second). A TotK save folder holds slot_00..slot_05\progress.sav (plus caption.sav,
 * footprint.sav, ...); that is how one is recognised wherever it comes from. */
#pragma once
#include <wchar.h>
#include <windows.h>

#define TOTK_TITLE_ID 0x0100F2C0115B6000ull

typedef struct SaveCandidate {
    wchar_t source[24];     /* "yuzu", "eden", "Ryujinx", "nxrecomp", "carpeta" (a folder the user picked) */
    wchar_t path[MAX_PATH]; /* the folder holding slot_0N\ */
    wchar_t user[40];       /* emulator user/profile folder ("" when none) */
    int slots;              /* slot_0N folders with a progress.sav */
    FILETIME newest;        /* newest progress.sav */
} SaveCandidate;

typedef struct SaveBackup {
    wchar_t path[MAX_PATH];
    wchar_t name[64];       /* save-YYYYMMDD-HHMMSS-NN-<why> */
    int slots;
    FILETIME newest;
} SaveBackup;

/* 1 when dir holds slot_0N\progress.sav; slots / newest (either may be NULL) describe it. */
int saves_is_save(const wchar_t *dir, int *slots, FILETIME *newest);

/* Saves of the emulators and our old runtime under appdata (normally %APPDATA%): yuzu and its forks
 * (<name>\nand\user\save\0000000000000000\<user>\0100F2C0115B6000), Ryujinx (Ryujinx\bis\user\save\<index>\0, the
 * title checked in ExtraData0 when present) and nxrecomp (nxrecomp\TotK\profile\save\<user>). Newest first. */
int saves_find(const wchar_t *appdata, SaveCandidate *out, int max);
/* Every save folder under root, at most depth levels down (a JKSV folder on an SD card, a portable emulator).
 * Newest first. */
int saves_find_under(const wchar_t *root, int depth, SaveCandidate *out, int max);

/* <base>\userdata\save, <base>\backups */
void saves_dir(const wchar_t *base, wchar_t *out, size_t n);
void saves_backups_dir(const wchar_t *base, wchar_t *out, size_t n);

/* Repairs an interrupted replace (saves_replace below): a leftover save.new is removed, a save.old with no save is
 * put back. Call at start. */
void saves_recover(const wchar_t *base);
/* Copies the current save to <base>\backups\save-<time>-NN-<why> (through a .tmp folder, renamed when complete).
 * Returns 1 and the backup's path, 0 when there is no save to back up, -1 on error. */
int saves_backup(const wchar_t *base, const wchar_t *why, wchar_t *out_path, size_t n);
/* Deletes all but the newest keep backups whose why is "auto"; other backups are kept. */
void saves_prune(const wchar_t *base, int keep);
/* Backups, newest first. */
int saves_list_backups(const wchar_t *base, SaveBackup *out, int max);
/* Makes src the save: copy to save.new, rename save to save.old, save.new to save, delete save.old. A failure at any
 * step leaves a complete save in place (the old one, or the new one). The caller makes a backup first.
 * 0 ok, else the failed step (1 copy, 2 move old away, 3 move new in). */
int saves_replace(const wchar_t *base, const wchar_t *src);

/* Recursive copy / delete (used above; also by tests). 0 ok. */
int saves_copy_tree(const wchar_t *src, const wchar_t *dst);
int saves_delete_tree(const wchar_t *dir);

/* Tests: make step k of saves_replace fail (0 = none). */
extern int g_saves_fail_step;
