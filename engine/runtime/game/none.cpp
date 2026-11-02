// game/none.cpp: the engine's game glue for a game with none of its own yet. A non-TotK build leaves out TotK's
// runtime sources and links this in their place: every function the generic engine calls into them answers as
// TotK's own code does with an unpatched main (no data page, no extra memory, no game screen known, no PC-scheme
// writer), so the generic paths run as for a stock image. Not compiled for TotK (outside runtime/*.cpp's glob). The
// names stay TotK's for now; a rename to game_* is a separate, pure change.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../totk_ability.h"
#include "../totk_input_cuts.h"
#include "../totk_menu_cursor.h"
#include "../totk_pcvk.h"
#include "../totk_player_state.h"
#include "../../../games/totk/include/totk/totk_patches.h"

using u64 = uint64_t;

// totk_camera_speed.cpp: no pack rebuilt (hle_fs.cpp), the mouse keeps its multiplier (pc_mouse.cpp)
bool totk_camera_pack_build(const std::vector<uint8_t>&, std::vector<uint8_t>*) { return false; }

extern "C" {
const char *totk_camera_pack_rel(void) { return nullptr; }
float totk_camera_stick_applied(void) { return 1.0f; }
// games/totk/engine/totk_engine.c (via nvn/totk_game_patches.c): boot, per-present step, extra pool memory
void totk_engine_boot(u64) {}
void totk_engine_present(void) {}
void totk_engine_interval(int) {}
u64 totk_engine_extra_memory(void) { return 0; }
// games/totk/runtime/totk_patches.c: the data page (NULL: no patched main), main's base, presets, the frame step
void *totk_data_page_current(void) { return nullptr; }
void *totk_main_base(void) { return nullptr; }
void totk_frame_step_retarget(void) {}
void totk_frame_step_hold(int) {}
int totk_max_fps_highest(void) { return 240; }
float totk_dynamic_res_current(void) { return 1.0f; }
const TotkQualityPreset *totk_quality_presets(unsigned *count) { if (count) *count = 0; return nullptr; }
// totk_aal_commands.cpp / totk_load_laps.cpp
bool totk_aal_cmd_hold_active(void) { return false; }
int totk_banc_change(void) { return 0; }
// totk_input_cuts (pc_input.cpp's game input cut): no cut known
int totk_input_reason(uint32_t) { return 0; }
// totk_menu_cursor.cpp: no game menu screen known, nothing posted to a UI thread
uint32_t totk_menu_screen(void) { return 0; }
u64 totk_menu_cursor_frame(void) { return 0; }
bool totk_menu_cursor_rstick(int32_t *) { return false; }
bool totk_ui_post(TotkUiTask, void *) { return false; }
// totk_pcvk.cpp: no PC virtual-key writer
void totk_pcvk_hold(uint64_t) {}
void totk_pcvk_tap(uint64_t) {}
void totk_pcvk_set_active(bool) {}
uint64_t totk_pcvk_frames(void) { return 0; }
// totk_player_state.cpp / totk_ability.cpp: the player is not known
uint32_t totk_player_state(void) { return 0; }
void totk_player_state_frame(void) {}
bool totk_player_request_ability(int) { return false; }
bool totk_ability_request(int) { return false; }
void dbatch_probe_frame(void) {}
}
