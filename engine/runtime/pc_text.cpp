// pc_text.cpp: pc_tr (pc_text.h). A table per language, English -> its text, looked up by the English text itself, so a
// row's label, help and choice names need no ids; a text a table lacks shows in English. Spanish (pc_text_es.inc,
// PC_ES lines: every text has one, tools/engine/check_texts.py), Russian, Brazilian Portuguese and Simplified Chinese
// (pc_text_ru.inc / _pt / _zh, PC_TR lines, pc-ui 11-02); pc_ui_lang (pc_lang.cpp) picks one. The few texts every
// player meets in any language (the first start's shader pass, the restart notice) have the game's other languages too
// (pc_text_langs.inc).
#include "pc_text.h"
#include "pc_lang.h"

#include <string.h>
#include <string_view>
#include <unordered_map>

namespace {

struct Pair { const char *en, *es; };
const Pair kEs[] = {
#define PC_ES(en, es) {en, es},
#include "pc_text_es.inc"
#undef PC_ES
    {nullptr, nullptr}};

const Pair kRu[] = {
#define PC_TR(en, t) {en, t},
#include "pc_text_ru.inc"
    {nullptr, nullptr}};
const Pair kPt[] = {
#include "pc_text_pt.inc"
    {nullptr, nullptr}};
const Pair kZh[] = {
#include "pc_text_zh.inc"
#undef PC_TR
    {nullptr, nullptr}};

using Table = std::unordered_map<std::string_view, const char *>;
Table make(const Pair *p) {
    Table m;
    for (; p->en; p++) m.emplace(p->en, p->es);
    return m;
}
// the table of the port's texts' language (pc_ui_lang), null: English
const Table *table() {
    static const Table *t = [] {
        const char *ui = pc_ui_lang();
        static Table m;
        if (!strcmp(ui, "es")) m = make(kEs);
        else if (!strcmp(ui, "ru")) m = make(kRu);
        else if (!strcmp(ui, "pt")) m = make(kPt);
        else if (!strcmp(ui, "zh-Hans")) m = make(kZh);
        else return (const Table *)nullptr;
        return (const Table *)&m;
    }();
    return t;
}

// pc_text_langs.inc: the other languages, in pc_lang_key()'s codes (PC_LANGS' order); cjk: the OSD's DejaVu font
// off Windows has no glyphs for it
struct LangKey { const char *key; bool cjk; };
const LangKey kLangKeys[] = {{"fr", false}, {"de", false}, {"it", false}, {"nl", false}, {"ru", false},
                             {"ja", true}, {"ko", true}, {"zh-Hans", true}, {"zh-Hant", true}};
constexpr int kNLangs = (int)(sizeof kLangKeys / sizeof kLangKeys[0]);
struct Langs { const char *en, *t[kNLangs]; };
const Langs kLangs[] = {
#define PC_LANGS(en, fr, de, it, nl, ru, ja, ko, zh_hans, zh_hant) {en, {fr, de, it, nl, ru, ja, ko, zh_hans, zh_hant}},
#include "pc_text_langs.inc"
#undef PC_LANGS
    {nullptr, {}}};

#ifdef _WIN32
constexpr bool kOsdHasCjk = true;
#else
constexpr bool kOsdHasCjk = false;
#endif

// the index in kLangKeys of `key`; -1: English or Spanish, or a cjk one the OSD cannot draw (it stays English)
int lang_index_of(const char *key, bool osd_cjk) {
    for (int j = 0; j < kNLangs; j++)
        if (!strcmp(key, kLangKeys[j].key)) return kLangKeys[j].cjk && !osd_cjk ? -1 : j;
    return -1;
}
int lang_index() {
    static const int i = lang_index_of(pc_lang_key(), kOsdHasCjk);
    return i;
}

const char *tr_langs(int li, const char *en) {
    if (li < 0) return en;
    for (const Langs *p = kLangs; p->en; p++)   // (a handful of texts: a scan)
        if (!strcmp(p->en, en)) return p->t[li];
    return en;
}

}  // namespace

extern "C" void pc_tr_each(void (*fn)(const char *text, void *ctx), void *ctx) {
    if (const Table *t = table())
        for (const auto &e : *t) fn(e.second, ctx);
}

extern "C" const char *pc_tr(const char *en) {
    if (!en) return en;
    if (const Table *t = table()) {
        auto it = t->find(en);
        if (it != t->end()) return it->second;
    }
    return tr_langs(lang_index(), en);
}

#ifdef NXR_PC_TEXT_TEST
// tools/engine/test_pc_settings.py: a language's text with or without the OSD's CJK glyphs (off Windows: none)
extern "C" const char *pc_tr_in(const char *key, int osd_cjk, const char *en) {
    return tr_langs(lang_index_of(key, osd_cjk != 0), en);
}
#endif
