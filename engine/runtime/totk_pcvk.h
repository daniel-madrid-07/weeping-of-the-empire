// totk_pcvk.h: the PC keys' player virtual keys. The game's player reads three u64 masks that the AI node
// ExecutePlayerCalcKey::updateImpl_ (main+0x1ccf894) writes from the pad once per frame. The hook on that node runs
// the original, then calls pc_vk_frame, which ORs in what the PC scheme holds, with the edges on the game's frame.
// The state lives in the runtime, so a runtime links against keys without the hook.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Router (input thread): the VK bits whose PC keys are down now (a level), and a tap: bits pressed at least once
// since the hook last ran, so a press shorter than a frame still gives one frame of press, then a release.
void totk_pcvk_hold(uint64_t mask);
void totk_pcvk_tap(uint64_t mask);
// A macro's own held bits, kept until it clears them (on = false): held in every frame between, as a G row's hold.
void totk_pcvk_macro_hold(uint64_t mask, bool on);
uint64_t totk_pcvk_held(void);   // the level last set (any thread)
// true while the PC scheme sends gameplay keys to the virtual keys (then those keys give no Npad bits). Off: the hook
// adds nothing.
void totk_pcvk_set_active(bool on);
bool totk_pcvk_active(void);
// The hook (game thread, once per run, after the original wrote the masks): ORs in the PC keys' bits, each mask & its
// keep. keep_*: ~0 normally, 0 while the game has the player's input off, and per mask the bits the game's own lockout
// keeps while it runs. The edge state follows the keys even while keep hides them, so a key held through an event does
// not fire when input returns.
void pc_vk_frame(uint64_t *trig, uint64_t *hold, uint64_t *release, uint64_t keep_trig, uint64_t keep_hold,
                 uint64_t keep_release);
// Pad bits the player must not see (any thread): pc_vk_frame clears these VK bits from the masks the original made from
// the pad, before it ORs in the PC keys. MAC_BOW_STRIP holds the pad's ZR for the UI (the bow page opens on the UI
// controller's ZR hold + d-right) while VK 13 stays masked, so Link does not aim
void totk_pcvk_pad_mask(uint64_t mask, bool on);
// Runs of the hook so far: the player's frames (0 without the hook). Macros count their steps in these.
uint64_t totk_pcvk_frames(void);
// true while the hook has not run for its gap (300 ms): a tap made now is dropped by its next, "resumed" frame
bool totk_pcvk_stale(void);

#ifdef __cplusplus
}
#endif
