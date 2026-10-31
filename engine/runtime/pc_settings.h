// pc_settings.h: nxrecomp's settings registry and settings.ini on the engine, shown by the F10 overlay
// (pc_overlay.cpp). Unless NXR_PC_OVERLAY=0 (then the engine's own recomp_settings.ini is in charge).
//
// The engine's knobs are environment variables read once (most in function-local statics, a few in global initializers),
// so the values reach the game through the environment: before any other static initializer (init_seg(lib)) the
// file is read and each row's value saved there goes into its variable. Rows the file does not mention leave the
// environment alone. TOTK_SETTINGS=0 (test runs) skips the file, as it does for the engine's. NX_SET_<KEY> (key upper-cased,
// '.' -> '_') sets a row for this run only and is never saved.
// Live rows have an apply callback (present thread) and take effect at once; PC_SET_RESTART rows at the next start.
//
// File: NXR_SETTINGS, else settings.ini next to the engine's recomp_settings.ini (TOTK_SETTINGS_FILE's folder, or the
// parent of TOTK_USERDATA, C:/totk by default). "key = value  # label" lines; comments and unknown lines are kept;
// written through a .tmp and a rename, 0.5 s after the last change and when the overlay closes. The first start
// without one imports recomp_settings.ini.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_SET_RESTART = 1,
    PC_SET_KEYS = 2,   // a key list ("E,Mouse Left", hle_hid.cpp's names; "" = the layout's own): values == NULL, no range
    PC_SET_PAD = 4,    // a pad-button list ("B,ZR", TOTK_PRESS's names; "" = the button's own, "none"): values == NULL
};

typedef struct pc_setting {
    const char *key;                 // "display.fps_cap"
    const char *page;                // "Graphics", "Controls", "Audio" or "Game"
    const char *section;             // heading on the page ("Display", "Rendering", ...)
    const char *label;
    const char *help;                // tooltip (NULL: none)
    const char *def;                 // default value
    const char *const *values;       // choice row: the stored values, NULL-terminated ...
    const char *const *names;        // ... and their labels
    float min, max, step;            // slider row (values == NULL): a number in [min, max]
    unsigned flags;                  // PC_SET_RESTART
    const char *env;                 // the variable the value goes into at start ("" removes it); NULL: none
    void (*to_env)(const char *value);        // instead of env (a value that sets several variables)
    void (*apply)(const char *value);         // live row: after a change, on the present thread
    const char *(*live_value)(void);          // the state as it is now when something else can change it (F11)
    const char *(*format)(float value);       // slider row: ImGui format for this value (NULL: "%.2f", or a
                                              // percentage for 0..1 rows); present thread, so it can read live state
} pc_setting;

// Adds rows (the array must outlive the process). Call from a static initializer: rows added before the start-up
// bridge get their file value into the environment there; rows added later get it at once.
// The overlay ships stand-in rows for knobs other workstreams own (window mode / size / presentation, volume, the
// controls, aniso, ...). Adding a row with the same key replaces the stand-in, keeping the saved value; any other
// duplicate key is ignored (the first row stays).
void pc_settings_add(const pc_setting *rows, int n);

// Graphics presets (the graphics.preset row: low, medium, high, ultra, ultra+, custom). Each entry gives one row's value
// in the five presets (NULL: the preset leaves that row alone); picking a preset sets them, and the row reads "custom"
// while the values match none. Rows that are not registered are skipped. A later entry for the same key replaces the
// earlier one (the table over the overlay's provisional values for the game-patch rows). The first-run screen offers
// Low.. Ultra only.
#define PC_PRESETS 5
typedef struct { const char *key; const char *value[PC_PRESETS]; } pc_preset_row;   // low, medium, high, ultra, ultra+
void pc_settings_add_presets(const pc_preset_row *rows, int n);

// Present thread (the overlay), and any thread once start-up is over for reading. NULL: no such key.
const char *pc_settings_get(const char *key);
bool pc_settings_copy(const char *key, char *out, size_t n);   // a row's value copied under the lock (false: no row)
void pc_settings_set(const char *key, const char *value);
// Any key: NX_SET_<KEY>, else the file's value, registered row or not (pc_input.cpp's per-context bindings)
bool pc_settings_raw(const char *key, char *out, size_t n);
// Any thread: writes (value NULL: removes) a key that has no registered row (pc_input.cpp's pc_bind_set:
// controls.<scope>.<action>), saved with the file at the next save; the file's other lines stay as they are
void pc_settings_raw_set(const char *key, const char *value);
// Any thread: saves a change still waiting for its 500 ms now (shim_win.c on WM_CLOSE: the game may exit before)
void pc_settings_save_pending(void);

#ifdef __cplusplus
}

// ---- for the overlay (pc_overlay.cpp)
struct pc_setting_view {
    const pc_setting *def;
    const char *value;
    bool restart_pending;    // a RESTART row changed since start
    bool overridden;         // NX_SET_<KEY>: this run only, not saved
};
int pc_settings_count();
pc_setting_view pc_settings_at(int i);
void pc_settings_tick(bool flush);   // each overlay frame: sync live values, save when due (flush: now)
void pc_settings_tick_save();        // each frame the overlay is closed: save when due (no row sync)
const char *pc_settings_file();      // NULL: no file (TOTK_SETTINGS=0 or overlay off)
bool pc_settings_first_run();        // a settings file is in use and this start found none (no import either)
int pc_settings_preset_index(const char *name);   // low 0 .. ultra+ 4, else -1
#endif
