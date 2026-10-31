// totk_input_cuts.h: the game's own input-cut reasons, as pc_ctx_decide needs them. The pause manager's flag set
// (the one ExecutePlayerCalcKey tests for input off) holds one bit per reason: "a game menu is up" 262e9f13 /
// 7f771e39, loading f6e0a4c8 (bit 43).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One reason looked up now, on the caller's thread (the pc_ctx, inside its pc_fault_guard bracket): the original's
// lookup with plain reads and no lock, so a table the game rehashes meanwhile can read wrong or fault. 1 set, 0 clear,
// -1 the table can't be read or the hash is not in it (unknown, never "clear"). Works in pause. A menu reason clear says
// nothing about the screen in front: shops, NPC talk and the quickmenus run with them clear.
int totk_input_reason(uint32_t hash);

#ifdef __cplusplus
}
#endif
