/* text_tr.h: the launcher's and the settings-before-play dialog's texts in Brazilian Portuguese (lang-ptbr, 11-02),
 * Russian and Simplified Chinese and Traditional Chinese, looked up by their English (text_pt.inc / text_ru.inc /
 * text_zh.inc / text_zht.inc, TR(L"english", L"translation") each). A text a table lacks shows in English, so a new
 * ES_EN / PP text never breaks the launcher; tools/engine/check_texts.py lists the missing ones and fails on a
 * translation with other printf specifiers or one no source text has.*/
#pragma once
#include <wchar.h>

/* the launcher's language (lang_init; preplay_dialog's es argument): 0 English, 1 Spanish (ES_EN's own), then a table */
enum { TR_EN, TR_ES, TR_PT, TR_RU, TR_ZH, TR_ZHT };

#define TR(en, tr) {en, tr},
static const wchar_t *const k_text_pt[][2] = {
#include "text_pt.inc"
    {0, 0}};
static const wchar_t *const k_text_ru[][2] = {
#include "text_ru.inc"
    {0, 0}};
static const wchar_t *const k_text_zh[][2] = {
#include "text_zh.inc"
    {0, 0}};
static const wchar_t *const k_text_zht[][2] = {
#include "text_zht.inc"
    {0, 0}};
#undef TR

static const wchar_t *tr_of(int lang, const wchar_t *en) {
    const wchar_t *const (*t)[2] = lang == TR_PT ? k_text_pt : lang == TR_RU ? k_text_ru : lang == TR_ZH ? k_text_zh
                                 : lang == TR_ZHT ? k_text_zht : 0;
    for (int i = 0; t && t[i][0]; i++)   /* (a few hundred texts, a few shown at a time: a scan) */
        if (!wcscmp(t[i][0], en)) return t[i][1];
    return en;
}
