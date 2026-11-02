// pc_amiibo.h: amiibo (nn::nfp) from the player's own dumps, and the F10 "Amiibo" tab that scans one (pc_overlay.cpp).
// The game sees one NFC reader; when it asks for a figure (its amiibo screen), the tab's Scan puts the picked dump on it.
#pragma once

bool pc_amiibo_tab_available();   // a launcher gave an amiibo folder (TOTK_AMIIBO_DIR)
void pc_amiibo_tab();             // the tab's contents (present thread, inside the tab item)
