/* text_pt.h: the launcher's and the settings-before-play dialog's texts in Brazilian Portuguese (lang-ptbr, 11-02),
 * looked up by their English (text_pt.inc, PT(L"english", L"portuguese") each). A text the table lacks shows in English,
 * so a new ES_EN / PP text never breaks the launcher; tools/engine/check_texts.py lists the missing ones and fails on a
 * translation with other printf specifiers or one no source text has. */
#pragma once
#include <wchar.h>

static const wchar_t *const k_text_pt[][2] = {
#define PT(en, pt) {en, pt},
#include "text_pt.inc"
#undef PT
    {0, 0}};

static const wchar_t *pt_of(const wchar_t *en) {
    for (int i = 0; k_text_pt[i][0]; i++)   /* (a few hundred texts, a few shown at a time: a scan) */
        if (!wcscmp(k_text_pt[i][0], en)) return k_text_pt[i][1];
    return en;
}
