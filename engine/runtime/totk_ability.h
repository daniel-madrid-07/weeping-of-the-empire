// totk_ability.h: select an ability by number, the way the L wheel commits it. The wheel
// (UIShortCutMainItemScreen::doOutBeforeImpl_, main+0x1acf574) writes the player's ability state through GameData on
// the UI thread; this replays it there, as a task posted to the UI hook (totk_ui_post, totk_menu_cursor.h). No guest
// symbol behind it, so it links against every key.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Any thread: ask for ability n (1 Ultrahand, 2 Fuse, 3 Ascend, 4 Recall, 5 Autobuild, 6 Camera); it is written at the
// next UI update. false: n out of range, or it cannot be queued (no UI hook in the key, the UI not updating: loading,
// a cutscene; totk_ui_post). Whether the ability is unlocked is decided there and logged.
bool totk_ability_request(int n);

#ifdef __cplusplus
}
#endif
