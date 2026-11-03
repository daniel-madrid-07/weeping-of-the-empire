/* preplay.h: the launcher's settings before play (preplay.c). */
#pragma once
#include <stddef.h>
#include <wchar.h>

/* settings.ini lines as the game writes them (pc_settings.cpp save_now: "key = value # label", UTF-8). ini_get: the
 * value of key (comment and blanks cut), 1 when the file has the key. ini_set: that line replaced, or added at the end
 * when the file lacks it (a missing file is made); every other line is kept as it is; written through a temporary
 * file. 1 on success. */
int ini_get(const wchar_t *path, const char *key, char *out, size_t n);
int ini_set(const wchar_t *path, const char *key, const char *value, const char *label);

/* The dialog: a few of the game's rows (window, resolution scale, frame rate, upscaler, ...) on the settings.ini at
 * ini, in Spanish when es is 1, in Brazilian Portuguese when 2 (text_pt.h). note: a line above the rows (why it opened;
 * NULL none). defaults: the build's own settings file (player.txt settings=), which "Reset all settings" puts back
 * (NULL: the file is removed, the game's defaults apply). 1: Play pressed (changes saved), 0: closed. */
int preplay_dialog(const wchar_t *ini, const wchar_t *defaults, int es, const wchar_t *title, const wchar_t *note);
