/* preplay.c: the launcher's settings before play (rival parity, PARITY.md 4.4; lead 2026-11-02). For a player whose game
 * closes while starting, or runs too slowly to reach F10: a few of the game's own rows (window, resolution scale, frame
 * rate, upscaler, safe mode,...) chosen before the start, written to the settings.ini the game reads at its start
 * (pc_settings.cpp). No settings of its own: the keys and values are the game's rows' (tools/engine/test_preplay_rows.py
 * checks them against pc_settings.cpp, pc_window.cpp and pc_dlss.cpp), only the rows changed here are written, and every
 * other line of the file stays as it is. Opened by Ctrl held at the launcher's start or --settings, and offered after a
 * start that failed (launcher.c).*/
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preplay.h"
#include "text_tr.h"

/* ---- settings.ini */

static char *read_all(const wchar_t *path, size_t *n) {
    *n = 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char *b = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        long sz = ftell(f);
        if (sz >= 0 && sz <= (4 << 20) && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)sz + 1))) {
            *n = fread(b, 1, (size_t)sz, f);
            b[*n] = 0;
        }
    }
    fclose(f);
    return b;
}
/* the line [l, e) sets key ("key = value", blanks around): *value after its '=' */
static int line_key(const char *l, const char *e, const char *key, const char **value) {
    if (e - l >= 3 && !memcmp(l, "\xEF\xBB\xBF", 3)) l += 3;   /* (a BOM before the first line) */
    while (l < e && (*l == ' ' || *l == '\t')) l++;
    size_t kl = strlen(key);
    if ((size_t)(e - l) < kl || memcmp(l, key, kl)) return 0;
    l += kl;
    while (l < e && (*l == ' ' || *l == '\t')) l++;
    if (l >= e || *l != '=') return 0;
    *value = l + 1;
    return 1;
}

int ini_get(const wchar_t *path, const char *key, char *out, size_t n) {
    size_t len;
    char *b = read_all(path, &len);
    out[0] = 0;
    if (!b) return 0;
    int found = 0;
    for (const char *l = b, *end = b + len; l < end;) {
        const char *e = memchr(l, '\n', (size_t)(end - l));
        if (!e) e = end;
        const char *v;
        if (!found && line_key(l, e, key, &v)) {   /* (the first one, as the game takes it) */
            const char *ve = v;
            while (ve < e && *ve != '#' && *ve != '\r') ve++;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            snprintf(out, n, "%.*s", (int)(ve - v), v);
            found = 1;
        }
        l = e < end ? e + 1 : end;
    }
    free(b);
    return found;
}

int ini_set(const wchar_t *path, const char *key, const char *value, const char *label) {
    size_t len;
    char *b = read_all(path, &len);
    const char *nl = b && strstr(b, "\r\n") ? "\r\n" : "\n";
    char line[512];
    snprintf(line, sizeof line, "%-34s = %-12s # %s%s", key, value, label ? label : "", nl);   /* (save_now's format) */
    size_t ln = strlen(line);
    char *out = malloc(len + ln + 4);
    if (!out) { free(b); return 0; }
    size_t o = 0;
    int done = 0;
    for (const char *l = b, *end = b + len; b && l < end;) {
        const char *e = memchr(l, '\n', (size_t)(end - l));
        const char *next = e ? e + 1 : end;
        const char *v;
        if (line_key(l, e ? e : end, key, &v)) {
            if (!done) {   /* the first: the new line, after a BOM it had; later copies of the key go */
                if (l == b && len >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3)) memcpy(out + o, b, 3), o += 3;
                memcpy(out + o, line, ln), o += ln;
                done = 1;
            }
        } else {
            memcpy(out + o, l, (size_t)(next - l)), o += (size_t)(next - l);
        }
        l = next;
    }
    if (!done) {
        if (o && out[o - 1] != '\n') memcpy(out + o, nl, strlen(nl)), o += strlen(nl);
        memcpy(out + o, line, ln), o += ln;
    }
    free(b);
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
    FILE *f = _wfopen(tmp, L"wb");
    int ok = f && fwrite(out, 1, o, f) == o;
    if (f) ok = !fclose(f) && ok;
    free(out);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

/* ---- the rows: key, the game's default (no line in the file), names in English / Spanish, the file's label */

typedef struct { const char *v; const wchar_t *en, *es; } Choice;
typedef struct { const char *key, *def, *label; const wchar_t *en, *es; const Choice *c; } Row;

static const Choice C_WINDOW[] = {
    {"windowed", L"Windowed", L"Ventana"},
    {"borderless", L"Borderless fullscreen (default)", L"Pantalla completa sin bordes (predeterminado)"},
    {"exclusive", L"Exclusive fullscreen", L"Pantalla completa exclusiva"}, {0}};
static const Choice C_SIZE[] = {
    {"", L"1280 x 720 (default)", L"1280 x 720 (predeterminado)"}, {"1600x900", L"1600 x 900", L"1600 x 900"},
    {"1920x1080", L"1920 x 1080", L"1920 x 1080"}, {"2560x1440", L"2560 x 1440", L"2560 x 1440"},
    {"3840x2160", L"3840 x 2160", L"3840 x 2160"}, {"2560x1080", L"2560 x 1080 (21:9)", L"2560 x 1080 (21:9)"},
    {"3440x1440", L"3440 x 1440 (21:9)", L"3440 x 1440 (21:9)"}, {"3840x1600", L"3840 x 1600 (21:9)", L"3840 x 1600 (21:9)"},
    {"5120x1440", L"5120 x 1440 (32:9)", L"5120 x 1440 (32:9)"},
    {"7680x1440", L"7680 x 1440 (48:9)", L"7680 x 1440 (48:9)"}, {0}};
static const Choice C_FPS[] = {
    {"30", L"30 (the game's own)", L"30 (el del juego)"}, {"40", L"40", L"40"}, {"45", L"45", L"45"}, {"60", L"60", L"60"},
    {"75", L"75", L"75"}, {"90", L"90", L"90"}, {"120", L"120", L"120"}, {"144", L"144", L"144"}, {"165", L"165", L"165"},
    {"180", L"180", L"180"}, {"200", L"200", L"200"}, {"240", L"240", L"240"},
    {"unlocked", L"Unlocked (monitor rate)", L"Sin límite (frecuencia del monitor)"},
    {"steady", L"Unlocked, steady (a rate it can hold)", L"Sin límite, estable (la que pueda mantener)"}, {0}};
static const Choice C_VSYNC[] = {
    {"", L"On (default, no tearing)", L"Activada (predeterminado, sin cortes)"},
    {"off", L"Off (tearing, lowest latency)", L"Desactivada (cortes, menos latencia)"}, {0}};
static const Choice C_SCALE[] = {
    {"0.67", L"67 %", L"67 %"}, {"0.8", L"80 %", L"80 %"}, {"0.9", L"90 %", L"90 %"},
    {"1", L"100 % (native)", L"100 % (nativa)"}, {"1.5", L"150 %", L"150 %"}, {"2", L"200 %", L"200 %"},
    {"auto", L"Follow the window height", L"Según la altura de la ventana"}, {0}};
static const Choice C_UPSCALER[] = {
    {"", L"Off", L"Desactivado"}, {"1", L"DLSS (NVIDIA RTX)", L"DLSS (NVIDIA RTX)"},
    {"dlaa", L"DLAA (NVIDIA RTX)", L"DLAA (NVIDIA RTX)"}, {"fsr", L"FSR (any GPU)", L"FSR (cualquier GPU)"}, {0}};
static const Choice C_FG[] = {
    {"", L"Off", L"Desactivada"}, {"1", L"DLSS x2", L"DLSS x2"}, {"2", L"DLSS x3", L"DLSS x3"}, {"3", L"DLSS x4", L"DLSS x4"},
    {"fsr", L"FSR x2", L"FSR x2"}, {0}};
static const Choice C_SHADER_WAIT[] = {
    {"", L"Limit to 0.1 s (default)", L"Hasta 0,1 s (predeterminado)"},
    {"always", L"Always wait (nothing missing)", L"Esperar siempre (no falta nada)"}, {0}};

static const Choice C_SAFE[] = {
    {"0", L"Off", L"Desactivado"},
    {"1", L"On: slower, for a game that crashes or hangs", L"Activado: más lento, si el juego se cierra o se cuelga"}, {0}};

static const Row ROWS[] = {
    {"display.window_mode", "borderless", "Window", L"Window", L"Ventana", C_WINDOW},
    {"display.window_size", "", "Window size", L"Window size (windowed)", L"Tamaño de la ventana", C_SIZE},
    {"display.fps_cap", "30", "Frame rate", L"Frame rate", L"Fotogramas por segundo", C_FPS},
    {"display.vsync", "", "Vertical sync", L"Vertical sync", L"Sincronización vertical", C_VSYNC},
    {"graphics.render_scale", "1", "Resolution scale", L"Resolution scale", L"Escala de resolución", C_SCALE},
    {"graphics.dlss", "", "Upscaler", L"Upscaler", L"Escalado", C_UPSCALER},
    {"graphics.frame_gen", "", "Frame generation", L"Frame generation", L"Generación de fotogramas", C_FG},
    {"graphics.shader_wait", "", "Shader wait", L"Waiting for new shaders", L"Espera a sombreadores nuevos", C_SHADER_WAIT},
    {"graphics.safe_mode", "0", "Safe mode (if the game crashes)", L"Safe mode", L"Modo seguro", C_SAFE},
};
enum { NROWS = sizeof ROWS / sizeof ROWS[0], ID_COMBO = 100, ID_RESET = 300 };

/* ---- the dialog, from a template made here (the launcher has no resources) */

static struct {
    const wchar_t *ini, *defaults, *title, *note;
    int es;   /* the texts: 0 English, 1 Spanish, 2 Brazilian Portuguese */
    char cur[NROWS][64];   /* each row's value in the file now (its default when absent) */
} g_pp;
#define PP(es_, en_) (g_pp.es == TR_ES ? (es_) : tr_of(g_pp.es, en_))   /* (TR_PT ...: text_tr.h) */

static WORD *align4(WORD *p) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD *put_str(WORD *p, const wchar_t *s) {
    do *p++ = (WORD)*s; while (*s++);
    return p;
}
static WORD *put_item(WORD *p, DWORD style, int x, int y, int cx, int cy, WORD id, WORD cls, const wchar_t *text) {
    p = align4(p);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = (short)x, it->y = (short)y, it->cx = (short)cx, it->cy = (short)cy;
    it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF, *p++ = cls;   /* 0x0080 button, 0x0082 static, 0x0085 combo box */
    p = put_str(p, text ? text : L"");
    *p++ = 0;   /* no creation data */
    return p;
}

static void fill(HWND dlg) {
    for (int i = 0; i < NROWS; i++) {
        const Row *r = &ROWS[i];
        if (!ini_get(g_pp.ini, r->key, g_pp.cur[i], sizeof g_pp.cur[i])) snprintf(g_pp.cur[i], sizeof g_pp.cur[i], "%s", r->def);
        HWND cb = GetDlgItem(dlg, ID_COMBO + i);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (int k = 0; r->c[k].v; k++) {
            LRESULT at = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)PP(r->c[k].es, r->c[k].en));
            SendMessageW(cb, CB_SETITEMDATA, (WPARAM)at, k);
            if (!strcmp(r->c[k].v, g_pp.cur[i])) sel = (int)at;
        }
        if (sel < 0) {   /* a value none of these is (a hand edit, a newer game's): kept unless another is picked */
            wchar_t t[96];
            swprintf(t, 96, PP(L"(actual: %hs)", L"(current: %hs)"), g_pp.cur[i]);
            sel = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(cb, CB_SETITEMDATA, (WPARAM)sel, (LPARAM)-1);
        }
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)sel, 0);
    }
}

static int save(HWND dlg) {
    int ok = 1;
    for (int i = 0; i < NROWS; i++) {
        HWND cb = GetDlgItem(dlg, ID_COMBO + i);
        LRESULT sel = SendMessageW(cb, CB_GETCURSEL, 0, 0);
        if (sel == CB_ERR) continue;
        LRESULT k = SendMessageW(cb, CB_GETITEMDATA, (WPARAM)sel, 0);
        if (k < 0) continue;
        const char *v = ROWS[i].c[k].v;
        if (!strcmp(v, g_pp.cur[i])) continue;   /* only what the player changed */
        ok = ini_set(g_pp.ini, ROWS[i].key, v, ROWS[i].label) && ok;
    }
    return ok;
}

/* "Reset all settings": settings.ini kept as settings-<date>.ini.bak beside it, the build's defaults copied back */
static void reset_all(HWND dlg) {
    if (MessageBoxW(dlg, PP(L"Se volverá a los ajustes con los que se instaló el juego (todos, también los de F10 y los "
                            L"controles). Tu archivo actual se guarda como copia junto a él.\n\n¿Continuar?",
                            L"Every setting goes back to how the game was installed (all of them, F10's and the controls "
                            L"too). Your current file is kept as a copy beside it.\n\nContinue?"),
                    g_pp.title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t bak[MAX_PATH + 32];
    swprintf(bak, MAX_PATH + 32, L"%.*ls-%04u%02u%02u-%02u%02u%02u.ini.bak", (int)(wcslen(g_pp.ini) - 4), g_pp.ini, t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (GetFileAttributesW(g_pp.ini) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(g_pp.ini, bak, MOVEFILE_REPLACE_EXISTING)) {
        MessageBoxW(dlg, PP(L"No se pudo apartar settings.ini (¿lo tiene abierto otro programa?).",
                            L"settings.ini could not be moved aside (is another program holding it?)."),
                    g_pp.title, MB_OK | MB_ICONWARNING);
        return;
    }
    if (g_pp.defaults && g_pp.defaults[0]) CopyFileW(g_pp.defaults, g_pp.ini, TRUE);
    fill(dlg);
}

static INT_PTR CALLBACK proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowTextW(dlg, g_pp.title);
        fill(dlg);
        SetForegroundWindow(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (!save(dlg))
                MessageBoxW(dlg, PP(L"No se pudieron guardar los ajustes en settings.ini.", L"The settings could not be saved to settings.ini."),
                            g_pp.title, MB_OK | MB_ICONWARNING);
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL: EndDialog(dlg, 0); return TRUE;
        case ID_RESET: reset_all(dlg); return TRUE;
        }
        break;
    }
    return FALSE;
}

/* the dialog's template for g_pp (test_preplay.c makes it hidden, without DialogBox) */
static DLGTEMPLATE *make_template(void) {
    const wchar_t *note = g_pp.note, *title = g_pp.title;
    static DWORD buf[4096];   /* (DWORD: the template's alignment) */
    memset(buf, 0, sizeof buf);
    const int W = 310, M = 7, LW = 112, CX = M + LW + 4, CW = W - CX - M, RH = 16;
    const int top = M + (note && note[0] ? 30 : 0), hint = top + NROWS * RH + 4, by = hint + 22, H = by + 14 + M;
    DLGTEMPLATE *d = (DLGTEMPLATE *)buf;
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = (WORD)(NROWS * 2 + 4 + (note && note[0] ? 1 : 0));
    d->cx = (short)W, d->cy = (short)H;
    WORD *p = (WORD *)(d + 1);
    *p++ = 0, *p++ = 0;   /* no menu, the standard dialog class */
    p = put_str(p, title);
    *p++ = 9;
    p = put_str(p, L"Segoe UI");
    if (note && note[0]) p = put_item(p, SS_LEFT, M, M, W - 2 * M, 26, 0xFFFF, 0x0082, note);
    for (int i = 0; i < NROWS; i++) {
        p = put_item(p, SS_LEFT, M, top + i * RH + 2, LW, 10, 0xFFFF, 0x0082, PP(ROWS[i].es, ROWS[i].en));
        p = put_item(p, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, CX, top + i * RH, CW, 160, (WORD)(ID_COMBO + i), 0x0085, NULL);
    }
    p = put_item(p, SS_LEFT, M, hint, W - 2 * M, 18, 0xFFFF, 0x0082,
                 PP(L"Se aplican al arrancar el juego. Todos los ajustes están en el juego con F10.",
                    L"They apply when the game starts. Every setting is in the game under F10."));
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, M, by, 100, 14, ID_RESET, 0x0080,
                 PP(L"Restablecer todos los ajustes...", L"Reset all settings..."));
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, W - M - 2 * 56 - 4, by, 56, 14, IDOK, 0x0080, PP(L"Jugar", L"Play"));
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, W - M - 56, by, 56, 14, IDCANCEL, 0x0080, PP(L"Cancelar", L"Cancel"));
    (void)p;
    return d;
}

int preplay_dialog(const wchar_t *ini, const wchar_t *defaults, int es, const wchar_t *title, const wchar_t *note) {
    g_pp.ini = ini, g_pp.defaults = defaults, g_pp.es = es, g_pp.title = title, g_pp.note = note;
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), make_template(), NULL, proc, 0) == 1;
}
