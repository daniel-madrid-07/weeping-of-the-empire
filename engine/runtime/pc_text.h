// pc_text.h: the PC port's own texts in the player's language: F10, the Welcome screen, the notices. The sources
// hold the English; pc_text_es.inc the Spanish of each (es / es-419, pc_lang.cpp); every other language reads the
// English. tools/engine/check_texts.py finds a text with no Spanish (and a Spanish no text uses).
#pragma once

// A text pc_tr gets later through a pointer (a table of labels): marks it for tools/engine/check_texts.py
#define PC_T(s) s

#ifdef __cplusplus
extern "C" {
#endif

// The text for the player: its Spanish when the game is in Spanish and pc_text_es.inc has it, else `en` itself (UTF-8;
// a format string keeps its specifiers in order). nullptr stays nullptr.
const char *pc_tr(const char *en);

#ifdef __cplusplus
}
#endif
