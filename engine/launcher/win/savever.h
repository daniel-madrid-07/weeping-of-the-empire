/* Saves of later game versions into 1.0.0, for the Windows launcher's import (nxrecomp, saves-compat). No UI:
 * launcher.c words the report, test_savever.c checks this against synthetic saves.
 *
 * The 1.0.0 game reads a save by key hash and skips keys it does not have, so a 1.1.x / 1.2.x save loads as it is
 * (integration, 2026-10-22). What it cannot take is what later versions made bigger or new: the caption picture's
 * room (20480 bytes in 1.4.x, 10240 in 1.0.0) and enum values 1.0.0 has no name for. The converter writes each file
 * exactly as 1.0.0 lays it out: the keys later versions added dropped (and their flags from the Bool64bitKey list),
 * the added enum values set to the enum's default, the picture fitted into 10240 bytes (WIC, scaled to 256x144), the
 * revision word and size of 1.0.0. The result must hold exactly 1.0.0's key set (a digest of it, from the GameDataList,
 * in savever_data.h), or the save is refused; the format and the tables: tools/saves/savever_gen.py. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct SaveKey { uint32_t group, hash; } SaveKey;
/* Enum (group 6) or EnumArray (group 7) key whose later versions added values: those become def. */
typedef struct SaveEnumMap { int sfi, group; uint32_t key, def; const uint32_t *added; int n_added; } SaveEnumMap;
/* Binary (group 18) key with room bytes after its u32 size in 1.0.0 (the caption picture). */
typedef struct SaveBinRoom { int sfi; uint32_t key, room; } SaveBinRoom;
/* One save file of 1.0.0: SaveFileIndex, name, size, key count and digest, the keys later versions added to it. */
typedef struct SaveFileSpec { int sfi; const wchar_t *name; uint32_t size, count; uint64_t digest; const SaveKey *drop; int n_drop; } SaveFileSpec;

typedef struct SaveTables {
    uint32_t target;                    /* the revision written: 1.0.0's */
    const uint32_t *later; int n_later; /* the revisions converted from */
    const SaveFileSpec *files; int n_files;
    const uint64_t *drop_b64; int n_b64; /* Bool64bitKey flags later versions added (progress.sav) */
    const SaveEnumMap *enums; int n_enums;
    const SaveBinRoom *bins; int n_bins;
    int pic_w, pic_h;                   /* the caption picture of 1.0.0 */
} SaveTables;
extern const SaveTables savever_tables;  /* the real ones (savever_data.h) */

enum { SV_SAME = 0, SV_CONVERTED = 1 };
enum { SV_BAD_FILE = -1, SV_UNKNOWN_VERSION = -2, SV_UNKNOWN_KEYS = -3, SV_TOO_BIG = -4, SV_PICTURE = -5, SV_IO = -6 };

typedef struct SaveConvReport {
    uint32_t from;          /* the newest revision converted from (0: nothing converted) */
    int files;              /* files rewritten */
    int keys_dropped;       /* table keys 1.0.0 does not have */
    int flags_dropped;      /* Bool64bitKey flags 1.0.0 does not have */
    int enums_reset;        /* enum values 1.0.0 does not have, set to the default */
    int pictures_fitted;    /* captions re-encoded smaller */
    int pictures_blank;     /* captions that could not be decoded, replaced with a black picture */
    int option_dropped;     /* 1: option.sav could not be converted and was left out (the game makes a new one) */
    int refused;            /* SV_* < 0 when the save was refused, else 0 */
    uint32_t detail;        /* SV_UNKNOWN_VERSION: the revision; SV_UNKNOWN_KEYS: the keys left over 1.0.0's count */
    wchar_t file[48];       /* the refused file, as slot_0N\progress.sav */
} SaveConvReport;

/* "1.0.0", "1.1.x / 1.2.x", "1.4.x"; NULL for a revision the tables do not know. */
const wchar_t *savever_name(uint32_t rev);
/* The revision word of a .sav file (0: unreadable or not a save). */
uint32_t savever_revision(const wchar_t *path);
/* The highest revision among dir\slot_0N\progress.sav (0: none). */
uint32_t savever_dir_revision(const wchar_t *dir);

/* One file. out has f->size bytes. SV_SAME (already 1.0.0: out untouched), SV_CONVERTED, or SV_* < 0 (r->refused,
 * r->detail set; out undefined). */
int savever_convert(const SaveTables *t, const SaveFileSpec *f, const uint8_t *in, size_t n, uint8_t *out, SaveConvReport *r);
/* Every file of a save folder (slot_0N\*.sav, option.sav), in place; all files or none (each is written through a
 * .tmp). SV_SAME, SV_CONVERTED or SV_* < 0 (nothing changed). An option.sav that cannot be converted is deleted
 * instead (r->option_dropped): the game writes its default settings. */
int savever_convert_dir(const SaveTables *t, const wchar_t *dir, SaveConvReport *r);

/* JPEG re-encode (WIC) into room bytes at w x h; jpg NULL: a black picture. 0 ok. */
int savever_fit_jpeg(const uint8_t *jpg, size_t n, int w, int h, uint8_t *out, size_t room, size_t *out_n);
