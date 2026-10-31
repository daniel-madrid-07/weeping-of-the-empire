// pc_text.cpp: pc_tr (pc_text.h). One table, English -> Spanish (pc_text_es.inc: PC_ES("English", "Español") lines,
// made and checked by tools/engine/check_texts.py), looked up by the English text itself, so a row's label, help and
// choice names need no ids; a text the table lacks shows in English. The few texts every player meets in any language
// (the first start's shader pass, the restart notice) have the game's other languages too (pc_text_langs.inc).
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

const std::unordered_map<std::string_view, const char *> &table() {
    static const auto t = [] {
        std::unordered_map<std::string_view, const char *> m;
        for (const Pair *p = kEs; p->en; p++) m.emplace(p->en, p->es);
        return m;
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

extern "C" const char *pc_tr(const char *en) {
    if (!en) return en;
    if (pc_lang_es()) {
        const auto &t = table();
        auto it = t.find(en);
        return it == t.end() ? en : it->second;
    }
    return tr_langs(lang_index(), en);
}

#ifdef NXR_PC_TEXT_TEST
// tools/engine/test_pc_settings.py: a language's text with or without the OSD's CJK glyphs (off Windows: none)
extern "C" const char *pc_tr_in(const char *key, int osd_cjk, const char *en) {
    return tr_langs(lang_index_of(key, osd_cjk != 0), en);
}
#endif
