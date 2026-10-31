// pc_lang.h: the game's language: nn::oe::GetDesiredLanguage's answer (hle_oe.cpp), so the game picks its message
// archive (USen, EUes...) from it, and the language of the PC port's own texts (pc_text.cpp). game.language: ""
// follows Windows' display language (the game's own if it has it, else English (US)), or one of the game's languages;
// a restart row. TOTK_LANG carries it (boot.py pins en-US for harness boots).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *pc_lang_code(void);   // the Switch language code: "en-US", "en-GB", "es", "es-419", "fr", "zh-Hans" ...
const char *pc_lang_key(void);    // pc_input.cpp's key-name languages: "en", "es", "fr", "de", "it", "nl", "ru", "ja",
                                  // "ko", "zh-Hans", "zh-Hant"
int pc_lang_es(void);             // the port's own texts in Spanish (es, es-419); else English

#ifdef __cplusplus
}
#endif
