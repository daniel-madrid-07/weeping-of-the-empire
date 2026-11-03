/* test_preplay.c: preplay.c's settings.ini edits (ini_get / ini_set) on files in %TEMP%: a line replaced in place in
 * the game's format, the other lines and comments kept byte for byte, CRLF kept, a BOM kept, a missing key added at the
 * end (after a last line without a newline), a key that only starts the same left alone, a commented-out line ignored,
 * later copies of a key dropped, a missing file made. Then the dialog made hidden (its template without WS_VISIBLE, no
 * DialogBox): every row filled from the file, a value no choice has kept as "(current:...)", and only the row the
 * player changed written.*/
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "preplay.c"   /* (its statics: make_template, proc, save) */

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static wchar_t g_path[MAX_PATH];
static void put(const char *s) {
    FILE *f = _wfopen(g_path, L"wb");
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}
static const char *get(void) {
    static char b[8192];
    FILE *f = _wfopen(g_path, L"rb");
    if (!f) return "";
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}
static const char *val(const char *key) {
    static char v[128];
    if (!ini_get(g_path, key, v, sizeof v)) return "<none>";
    return v;
}

int main(void) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_path, MAX_PATH, L"%lsnxr_preplay_%lu.ini", tmp, GetCurrentProcessId());
    DeleteFileW(g_path);

    put("# Settings (written by the game)\n"
        "display.fps_cap                    = 120          # Frame rate\n"
        "display.fps_cap_hint               = 1            # another key that starts the same\n"
        "# display.window_mode = windowed\n"
        "graphics.dlss                      =              # Upscaler\n"
        "game.language                      = es           # Language");
    CHECK(!strcmp(val("display.fps_cap"), "120"), "fps %s", val("display.fps_cap"));
    CHECK(!strcmp(val("graphics.dlss"), ""), "dlss '%s'", val("graphics.dlss"));
    CHECK(!strcmp(val("display.window_mode"), "<none>"), "commented line read: %s", val("display.window_mode"));
    CHECK(!strcmp(val("game.language"), "es"), "lang %s", val("game.language"));

    CHECK(ini_set(g_path, "display.fps_cap", "40", "Frame rate"), "set fps");
    CHECK(ini_set(g_path, "display.window_mode", "windowed", "Window"), "set window");
    CHECK(!strcmp(get(), "# Settings (written by the game)\n"
                         "display.fps_cap                    = 40           # Frame rate\n"
                         "display.fps_cap_hint               = 1            # another key that starts the same\n"
                         "# display.window_mode = windowed\n"
                         "graphics.dlss                      =              # Upscaler\n"
                         "game.language                      = es           # Language\n"
                         "display.window_mode                = windowed     # Window\n"),
          "file:\n%s", get());
    CHECK(!strcmp(val("display.fps_cap_hint"), "1"), "hint %s", val("display.fps_cap_hint"));

    put("\xEF\xBB\xBF" "display.vsync = off\r\ngraphics.render_scale = auto\r\ndisplay.vsync = \r\n");
    CHECK(!strcmp(val("display.vsync"), "off"), "bom + first copy: %s", val("display.vsync"));
    CHECK(ini_set(g_path, "display.vsync", "", "Vertical sync"), "set vsync");
    CHECK(!strcmp(get(), "\xEF\xBB\xBF" "display.vsync                      =              # Vertical sync\r\n"
                         "graphics.render_scale = auto\r\n"),
          "crlf / bom / copies:\n%s", get());
    CHECK(ini_set(g_path, "graphics.frame_gen", "fsr", "Frame generation"), "add crlf");
    CHECK(strstr(get(), "auto\r\ngraphics.frame_gen                 = fsr          # Frame generation\r\n") != NULL,
          "added line keeps CRLF:\n%s", get());

    DeleteFileW(g_path);
    CHECK(!strcmp(val("display.fps_cap"), "<none>"), "no file");
    CHECK(ini_set(g_path, "display.fps_cap", "60", "Frame rate"), "make file");
    CHECK(!strcmp(get(), "display.fps_cap                    = 60           # Frame rate\n"), "new file:\n%s", get());

    put("display.fps_cap = 120\ngraphics.render_scale = 0.7\nother.key = x\n");
    g_pp.ini = g_path, g_pp.defaults = NULL, g_pp.es = 0, g_pp.title = L"test", g_pp.note = L"a note";
    HWND dlg = CreateDialogIndirectParamW(GetModuleHandleW(NULL), make_template(), NULL, proc, 0);
    CHECK(dlg != NULL, "dialog not made (error %lu)", GetLastError());
    if (dlg) {
        CHECK(!IsWindowVisible(dlg), "dialog visible");
        int fps = -1, scale = -1;
        for (int i = 0; i < NROWS; i++) {
            if (!strcmp(ROWS[i].key, "display.fps_cap")) fps = i;
            if (!strcmp(ROWS[i].key, "graphics.render_scale")) scale = i;
            int n = 0;
            while (ROWS[i].c[n].v) n++;
            HWND cb = GetDlgItem(dlg, ID_COMBO + i);
            LRESULT items = SendMessageW(cb, CB_GETCOUNT, 0, 0);
            CHECK(cb && items == n + (i == scale), "%s: %d items, %d choices", ROWS[i].key, (int)items, n);
            CHECK(SendMessageW(cb, CB_GETCURSEL, 0, 0) != CB_ERR, "%s: nothing selected", ROWS[i].key);
        }
        CHECK(fps >= 0 && scale >= 0, "rows");
        HWND cf = GetDlgItem(dlg, ID_COMBO + fps), cs = GetDlgItem(dlg, ID_COMBO + scale);
        LRESULT k = SendMessageW(cf, CB_GETITEMDATA, SendMessageW(cf, CB_GETCURSEL, 0, 0), 0);
        CHECK(k >= 0 && !strcmp(C_FPS[k].v, "120"), "fps shows %d", (int)k);
        CHECK(SendMessageW(cs, CB_GETITEMDATA, SendMessageW(cs, CB_GETCURSEL, 0, 0), 0) == -1, "0.7 not kept as current");
        for (LRESULT i = 0; i < SendMessageW(cf, CB_GETCOUNT, 0, 0); i++)
            if (!strcmp(C_FPS[SendMessageW(cf, CB_GETITEMDATA, i, 0)].v, "60")) SendMessageW(cf, CB_SETCURSEL, i, 0);
        CHECK(save(dlg), "save");
        CHECK(!strcmp(get(), "display.fps_cap                    = 60           # Frame rate\n"
                             "graphics.render_scale = 0.7\nother.key = x\n"),
              "only the changed row written:\n%s", get());
        DestroyWindow(dlg);
    }

    DeleteFileW(g_path);
    printf("preplay: %s\n", g_fail ? "fail" : "ok");
    return g_fail ? 1 : 0;
}
