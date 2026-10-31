// pc_gameplay.cpp: the gameplay half of the PC controls, registered with the router (pc_input.h) at start:
//  - which gameplay context a player state is (4.12's priority), for pc_ctx_player's push;
//  - the macros of the 'M' rows: short pad sequences (dodge, parry, the bow's shots, a weapon step) and pad holds the
//    mouse shapes while their key is down (Ultrahand's rotation). The ability wheel and the strips are plain holds;
//    the quickmenu context feeds them the mouse (4.13). One runs at a time (the router's rule); steps count game
//    frames, at least 2 per step: the game reads the pad once a frame, so a 1-frame press can be missed;
//  - the virtual-key writer of the 'G' rows: totk_pcvk.cpp, read by the ExecutePlayerCalcKey hook.
#include "pc_gameplay.h"
#include "pc_input.h"
#include "pc_settings.h"
#include "totk_ability.h"
#include "totk_pcvk.h"
#include "totk_player_state.h"
#include "totk_menu_cursor.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// hle_hid.cpp's action bits (pc_input.cpp's B_* values)
constexpr uint64_t kA = 1u << 0, kB = 1u << 1, kX = 1u << 2, kY = 1u << 3, kL = 1u << 6, kR = 1u << 7, kZL = 1u << 8, kZR = 1u << 9,
                   kDL = 1u << 12, kDU = 1u << 13, kDR = 1u << 14, kDD = 1u << 15;
constexpr int32_t kStickMax = 32767;
// controls.ability.rotate_step (TOTK_PC_ROTATE_STEP, live): mouse counts per Ultrahand / Autobuild rotation step
// (3.5: 40 by default)
std::atomic<int> g_rotate_step{40};
const char *const kSteps[] = {"20", "30", "40", "60", "80", nullptr};
const char *const kStepNames[] = {"20 (fast)", "30", "40", "60", "80 (slow)", nullptr};
void apply_step(const char *v) { const int n = v ? atoi(v) : 0; g_rotate_step.store(n >= 5 && n <= 400 ? n : 40); }
const pc_setting kRows[] = {
    {"controls.ability.rotate_step", "Controls", "Keyboard and mouse", "Ultrahand rotation step",
     "TOTK_PC_ROTATE_STEP: mouse counts per 45-degree step while R is held in Ultrahand or Autobuild.", "40", kSteps,
     kStepNames, 0, 0, 0, 0, "TOTK_PC_ROTATE_STEP", nullptr, apply_step, nullptr, nullptr},
};
[[maybe_unused]] const bool g_rows = (pc_settings_add(kRows, 1), true);
int step_px() {
    static std::atomic<bool> read{false};
    if (!read.exchange(true)) if (const char *e = getenv("TOTK_PC_ROTATE_STEP")) apply_step(e);
    return g_rotate_step.load();
}
constexpr int kVkF = 0x46, kVkQ = 0x51, kVkEscape = 0x1b;
// Appends an action's keys at a context's level to k[n..4) and returns the new count: the rebinding API (9be86d7;
// US-position VKs, as in->down takes them), `dflt` while it is not live (PCB_NOT_READY). An explicit unbind adds
// nothing. Only keys and buttons (a wheel notch has no hold); never `skip` (the macro's own trigger: an attack key
// equal to aim's would hold the bow in its wait forever), no key twice.
int add_keys(int ctx, const char *action, int dflt, int skip, int k[4], int n) {
    int b[4];
    const int got = pc_bind_get(ctx, action, b, 4, nullptr);
    if (got < 0) { b[0] = dflt; }
    for (int i = 0; i < (got < 0 ? 1 : got); i++) {
        const int v = b[i];
        bool dup = v <= 0 || v >= 256 || v == skip;
        for (int j = 0; j < n && !dup; j++) dup = k[j] == v;
        if (!dup && n < 4) k[n++] = v;
    }
    return n;
}

// MAC_BOW runs with their key held past the charge wait (the draw): AIM needs one. UseBow (59) stays in slot 1 for as
// long as the bow is in Link's hand, drawn or not (DrawnBow 205 never shows), so PCP_AIM alone kept AIM after the
// first draw and V / Q / Esc dead
std::atomic<int> g_bow_held{0};
// The first draw / the last let-go: the HUD bar's set named at once, where the draw is AIM (from_player: on foot, in the
// air, swimming; the horse, a vehicle, the glider keep theirs). AIM's bar keys B as Esc: without it the bar drew on foot's
// Shift for the 30 ms debounce plus its 100 ms hold, then Esc, and back after a shot
static void bow_glyphs(bool edge, bool drawn) {
    if (!edge) return;
    const int c = pc_ctx();
    if (drawn ? c == PCX_WORLD || c == PCX_AIR || c == PCX_SWIM : c == PCX_AIM) pc_glyph_intent(drawn ? PCX_AIM : PCX_WORLD, 0);
}

int from_player(uint32_t f, uint8_t ability) {
    (void)ability;   // (one ABILITY context but the camera; the macros branch on pc_ctx_ability())
    if (f & PCP_LOADING) return PCX_LOADING;
    if (f & PCP_EVENT) return PCX_CUTSCENE;
    // (aiming from the saddle, a vehicle or the glider stays in that context, so Shift still spurs, E still
    // dismounts, Space still closes the glider; AIM is on foot, in the air or swimming)
#ifdef PC_HAS_CAMERA
    if ((f & PCP_ABILITY) && ability == PCA_CAMERA) return PCX_CAMERA;   // (the Purah Pad camera in use)
#endif
    if (f & PCP_ABILITY) return PCX_ABILITY;
    if (f & PCP_HORSE) return PCX_HORSE;
    if (f & PCP_CONSTRUCT) return PCX_CONSTRUCT;   // (before VEHICLE: the construct's own rows)
    if (f & PCP_VEHICLE) return PCX_VEHICLE;
    if (f & PCP_GLIDE) return PCX_GLIDE;
    if ((f & PCP_AIM) && g_bow_held.load(std::memory_order_relaxed) > 0) return PCX_AIM;
    if (f & PCP_SWIM) return PCX_SWIM;
    if (f & PCP_CLIMB) return PCX_CLIMB;
    if (f & PCP_AIR) return PCX_AIR;
    return PCX_WORLD;
}

// The running macros, one per key that started one (the router runs up to 4 at once, by input: the bow held while a
// dodge or a parry runs). Only the input thread touches them.
struct Run {
    int id = MAC_NONE, input = 0;
    uint64_t start = 0;             // game frame of the press
    bool first = true;
    bool released = false; uint64_t released_at = 0;
    bool guarding = false;          // MAC_PARRY: the shield was already up
    bool waiting = false, held = false;   // MAC_BOW: an attack charge to let finish first; counted in g_bow_held
    bool cancel_was = false; uint64_t cancel_until = 0;   // the bow's draw on the horse: the cancel key's edge, B pulse
    bool bow_mode = false;          // MAC_ATTACK: the bow in hand at the press (the draw), else the attack
    uint64_t held_vk = 0;           // MAC_ATTACK / MAC_SPRINT: the VK held in the hook's macro word
                                    // (cleared on drop)
    bool vk_deferred = false; uint64_t vk_frame0 = 0, vk_at = 0;   // MAC_ATTACK: VK 9 waiting for the hook's resume
    uint64_t vk_rel_ms = 0;         // MAC_ATTACK: when LMB was seen up (the 2-frame minimum hold ends 500 ms after it)
    bool strip_only = false;        // MAC_BOW_STRIP: LMB's bow already drawn (d-right only)
    bool vk13_masked = false;       // MAC_BOW_STRIP: the pad's ZR hidden from the player (cleared on drop too)
    bool on_stick = false;          // MAC_VEHICLE_LEAVE: on a ride at the press
    bool q_stop = false; uint64_t q_at = 0;   // MAC_HOLSTER: the take-out waits for Link to stand (the movement held at 0)
    unsigned cx_was = 0;            // MAC_CONSTRUCT_ATTACH: the claimed keys down at the last tick (edges pick)
    uint64_t cx_end = 0;            // MAC_CONSTRUCT_*: the pad's B held until this frame (the cancel / the get-off)
    bool cx_seen = false;           // MAC_CONSTRUCT_ATTACH: the attach screen has been seen open
    uint64_t cx_ms = 0;             // MAC_CONSTRUCT_ATTACH: E's press (now_ms; the attach screen's opening wait)
    double ax = 0, ay = 0;          // accumulated mouse
    int queue_x = 0, queue_y = 0;   // strip flicks / rotation steps still to play
    int step_dir_x = 0, step_dir_y = 0; uint64_t step_at = 0;   // the one being played
};
constexpr int kRuns = 8;
Run g_runs[kRuns];
// MAC_BOW_TOGGLE's accepted take-out request (now_ms, 0 none): the bow shows in hand ~2 game frames later, and an LMB
// in between must still draw, not swing (MAC_ATTACK). Cleared when the bow shows, at a put-away, or after kBowSoonMs
uint64_t g_bow_asked_ms;
constexpr uint64_t kBowSoonMs = 150;
// Mineru's construct: the attach window MAC_CONSTRUCT_ATTACH opens (now_ms it closes by, 0 closed); a pick, a cancel or
// another context closes it first. Long enough to aim at a device and choose the part
uint64_t g_attach_until_ms;
constexpr uint64_t kAttachMs = 5000;
constexpr uint64_t kAttachOpenMs = 700;   // after E the attach screen opens within this, or the X was the construct's jump
constexpr int kVkLButton = 0x01, kVkRButton = 0x02;
Run *run_of(int input, bool make) {
    for (auto &r : g_runs) if (r.id != MAC_NONE && r.input == input) return &r;
    if (!make) return nullptr;
    for (auto &r : g_runs) if (r.id == MAC_NONE) return &r;
    return &g_runs[kRuns - 1];
}

bool key_down(const PcMacroIn *in) { return in->down(in->input, in->user); }

void rstick(PcMacroOut *o, double x, double y) {
    o->stick[1] = true;
    o->sx[1] = (int32_t)lrint(x * kStickMax);
    o->sy[1] = (int32_t)lrint(y * kStickMax);
}
// A queued step (strip flick, rotation step): 2 frames on, 2 frames off. dx / dy: the step being played, if any.
bool step(Run &r, uint64_t f, int *dx, int *dy) {
    if ((r.step_dir_x || r.step_dir_y) && f - r.step_at >= 4) r.step_dir_x = r.step_dir_y = 0;
    if (!r.step_dir_x && !r.step_dir_y && (r.queue_x || r.queue_y)) {
        if (r.queue_x) { r.step_dir_x = r.queue_x > 0 ? 1 : -1; r.queue_x -= r.step_dir_x; }
        else { r.step_dir_y = r.queue_y > 0 ? 1 : -1; r.queue_y -= r.step_dir_y; }
        r.step_at = f;
    }
    const bool on = (r.step_dir_x || r.step_dir_y) && f - r.step_at < 2;
    *dx = on ? r.step_dir_x : 0; *dy = on ? r.step_dir_y : 0;
    return r.step_dir_x || r.step_dir_y || r.queue_x || r.queue_y;
}
void count_steps(Run &r, double dx, double dy, bool with_y) {
    const double step = step_px();
    r.ax += dx; r.ay += with_y ? dy : 0;
    while (r.ax >= step) { r.queue_x++; r.ax -= step; }
    while (r.ax <= -step) { r.queue_x--; r.ax += step; }
    while (r.ay >= step) { r.queue_y--; r.ay -= step; }   // the mouse down: d-pad down
    while (r.ay <= -step) { r.queue_y++; r.ay += step; }
}

bool tick_run(Run &r, const PcMacroIn *in, PcMacroOut *o);
bool tick(const PcMacroIn *in, PcMacroOut *o) {
    Run *rp = run_of(in->input, false);
    if (!rp) return false;
    Run &r = *rp;
    const bool more = tick_run(r, in, o);
    if (!more) r = Run();
    return more;
}

bool tick_run(Run &r, const PcMacroIn *in, PcMacroOut *o) {
    if (r.first) r.start = in->frame;
    const uint64_t f = in->frame, d = f - r.start;
    const bool held = key_down(in);
    if (!held && !r.released) { r.released = true; r.released_at = f; }
    const bool first = r.first;
    r.first = false;
    switch (r.id) {
    case MAC_DODGE:
        if (d < 2) { o->bits |= kZL; return true; }
        if (d < 4) { o->bits |= kZL | kX; return true; }
        return false;
    case MAC_PARRY:
        // the guard key held (its VK 27, guard only: lock-on alone is VK 2 with the shield down, G1): then A alone,
        // else ZL, then ZL + A (GuardKeep 248 does not mark the shield up). Before the VK hook runs there is no
        // published hold word, so F always takes the ZL path
        if (first) r.guarding = (totk_pcvk_held() & (1ull << 27)) != 0;
        if (r.guarding) { if (d < 2) { o->bits |= kA; return true; } return false; }
        if (d < 4) { o->bits |= kZL; return true; }
        if (d < 6) { o->bits |= kZL | kA; return true; }
        return false;
    case MAC_BOW: {
        // LMB has no part in it Esc in AIM is the cancel (a B pulse, the AIM "quit" row). On a horse, the glider or a
        // vehicle the aim stays in that context, where B is Back: there the release is the only way out, and it
        // shoots
        if (!r.released) {
            int k[4];
            // the attack keys of the contexts the draw starts in (WORLD, AIR: F unless rebound; the defaults while
            // rebinding is not live)
            int nk = add_keys(PCX_WORLD, "attack", kVkF, in->input, k, 0);
            nk = add_keys(PCX_AIR, "attack", kVkF, in->input, k, nk);
            bool charge = false;
            for (int i = 0; i < nk; i++) charge = charge || in->down(k[i], in->user);
            // RMB pressed while an attack key still charges: the bow waits until it is let go, so the charge ends as
            // the player meant
            if (first) r.waiting = charge;
            if (r.waiting) {
                if (charge) return true;
                r.waiting = false;
            }
            o->bits |= kZR;
            if (!r.held) { r.held = true; bow_glyphs(g_bow_held.fetch_add(1, std::memory_order_relaxed) == 0, true); }
            // the claim: the attack keys stay off until the bow is down, so F doesn't swing mid-draw
            int n = 0;
            for (int i = 0; i < nk; i++) o->claim[n++] = k[i];
            // on the horse the aim stays HORSE's, where Esc is the pause: while drawn there the cancel key (AIM "quit",
            // Esc unless rebound) is the bow's, and a fresh press is a 3-frame B that lowers it without a shot. Claimed
            // every tick, so the pause never sees it while the bow is up
            if (totk_player_state() & TPS_HORSE) {
                int c[4];
                const int nc = add_keys(PCX_AIM, "quit", kVkEscape, in->input, c, 0);
                bool cancel = false;
                for (int i = 0; i < nc; i++) cancel = cancel || in->down(c[i], in->user);
                if (cancel && !r.cancel_was) r.cancel_until = f + 3;
                r.cancel_was = cancel;
                for (int i = 0; i < nc && n < 4; i++) o->claim[n++] = c[i];
            }
            if (f < r.cancel_until) o->bits |= kB;
            return true;
        }
        if (r.held) {
            r.held = false;
            bow_glyphs(g_bow_held.fetch_sub(1, std::memory_order_relaxed) == 1, false);
            static int logs;
            if (logs++ < 100) printf("[pc] bow release: ZR up (the shot)\n");
        }
        return false;
    }
    case MAC_BOW_DRAW:
    case MAC_ATTACK: {
        // LMB attacks, and with the bow OUT it draws instead. 4.26: the bow in hand NOW, the game's own state
        // (TPS_BOW_OUT), decides; F takes the bow out and puts it away, so the class latch of 4.23 (the last strip
        // pick) no longer chooses here: with no bow in hand LMB is the sword (VK 9, which draws a sheathed one, the
        // pad's Y). Decided at the press. MAC_BOW_DRAW, F's hold: the bow mode below from the start, the pad's ZR draws
        // whatever is in hand, as LMB's draw it counts in g_bow_held (AIM's rows and glyphs on foot and in the air; the
        // horse's Esc lowers it)
        if (first && r.id == MAC_BOW_DRAW) {
            r.bow_mode = (totk_player_state() & TPS_VALID) != 0;
            static int logs;
            if (logs++ < 100) printf("[pc] bow draw (F held): %s\n", r.bow_mode ? "ZR" : "nothing (no player)");
        } else if (first) {
            const uint32_t st = totk_player_state();
            // (or F's take-out still on its way: ZR then is harmless, its trigger is the game's own take-out input)
            const bool soon = g_bow_asked_ms && in->now_ms - g_bow_asked_ms < kBowSoonMs && (st & TPS_BOW_EQUIPPED);
            if ((st & TPS_BOW_OUT) || !soon) g_bow_asked_ms = 0;
            // the glider: "attack: VK 9 (no bow in hand)" gliding) and the pad's ZR draws from it, in slow motion, so a
            // bow equipped is enough there, whether or not F's take-out (MAC_BOW_OUT) has brought the bow out. The skydive
            // too (AIR with slot 0's verb Glide 5 / AirDive 19), SelectorPlayerSchedulerAirDive / ParasailGlide enter the
            // slow-motion aim only on VK 13 held (or UseBow already in slot 1), so F's bit 21 alone does not, and no
            // skydive reader takes the attack's VK 9. A plain jump / fall keeps LMB's plunge attack (Y)
            const int v0 = totk_player_verb(0);
            const bool skydive = in->ctx == PCX_AIR && (v0 == 5 || v0 == 19);
            const bool glide = (in->ctx == PCX_GLIDE || skydive) && (st & TPS_BOW_EQUIPPED);
            r.bow_mode = (st & TPS_VALID) && ((st & TPS_BOW_OUT) || soon || glide);
            // the decision, for the checker (the route line reads "attack" both ways)
            static int logs;
            if (logs++ < 200) printf("[pc] attack: %s\n", r.bow_mode ? "the bow in hand, LMB draws" : "VK 9 (no bow in hand)");
        }
        if (r.bow_mode) {
            // the bow's draw, as MAC_BOW: ZR while LMB is held (AIM context), the release is ZR up: the shot
            if (!r.released) {
                o->bits |= kZR;
                if (!r.held) { r.held = true; bow_glyphs(g_bow_held.fetch_add(1, std::memory_order_relaxed) == 0, true); }
                // on the horse the aim stays HORSE's, where Esc is the pause: while drawn there the cancel key (AIM
                // "quit", Esc unless rebound) is the bow's, and a fresh press is a 3-frame B that lowers it without a
                // shot. Claimed every tick, so the pause never sees it while the bow is up. (178d801b had this in
                // MAC_BOW, RMB's draw; since 4.23 the draw is LMB's, here.)
                if (totk_player_state() & TPS_HORSE) {
                    int c[4];
                    const int nc = add_keys(PCX_AIM, "quit", kVkEscape, in->input, c, 0);
                    bool cancel = false;
                    for (int i = 0; i < nc; i++) cancel = cancel || in->down(c[i], in->user);
                    if (cancel && !r.cancel_was) r.cancel_until = f + 3;
                    r.cancel_was = cancel;
                    for (int i = 0; i < nc && i < 4; i++) o->claim[i] = c[i];
                }
                if (f < r.cancel_until) o->bits |= kB;
                return true;
            }
            if (r.held) {
                r.held = false;
                bow_glyphs(g_bow_held.fetch_sub(1, std::memory_order_relaxed) == 1, false);
                static int logs;
                if (logs++ < 100) printf("[pc] bow release: ZR up (the shot)\n");
            }
            return false;
        }
        // the attack: VK 9 held in the hook's macro word from the press to the release (a tap swings, a hold charges);
        // plus a latched tap, so a click pressed and released between two game frames still swings (G rows survive
        // this through the same tap) If the hook is stale at the press (a strip / menu just closed: the player's calc
        // did not run), its next frame is a "resumed" one that drops the taps made meanwhile and counts VK 9 held
        // across it as held before, so the press never reached the game. Then VK 9 waits for that frame (the hook's
        // frame count moves; cap 500 ms) and is tapped and held from the next one, for at least 2 hook frames even
        // when LMB is already up (a click replayed after its release)
        if (first) {
            r.vk_frame0 = totk_pcvk_frames();
            r.vk_deferred = totk_pcvk_stale();
            r.vk_at = in->now_ms;
        }
        if (r.vk_deferred) {
            if (totk_pcvk_frames() == r.vk_frame0) {
                if (in->now_ms - r.vk_at < 500) return true;
                // (the hook never resumed: a press made during a longer stop, a menu or an event, is not played late;
                // nothing was tapped or held, test_pc_attack.py "stalled")
                static int drop_logs;
                if (drop_logs++ < 100) printf("[pc] attack: VK 9 dropped (the hook did not resume in 500 ms)\n");
                return false;
            }
            r.vk_deferred = false;
            r.vk_frame0 = totk_pcvk_frames();   // (from here: the frames VK 9 has been held for)
            static int logs;
            if (logs++ < 100) printf("[pc] attack: VK 9 after the hook's resume (%llu ms)\n", (unsigned long long)(in->now_ms - r.vk_at));
            totk_pcvk_tap(1ull << 9);
            totk_pcvk_macro_hold(1ull << 9, true);
            r.held_vk = 1ull << 9;
            return true;
        }
        if (first) {
            totk_pcvk_tap(1ull << 9);
            totk_pcvk_macro_hold(1ull << 9, true);
            r.held_vk = 1ull << 9;
        }
        if (!r.released) return true;
        // (the 2-frame minimum hold, capped 500 ms after the release: if the hook stops again right after the press, a
        // menu or event halting the player's calc in the same context, its frame count never moves, and VK 9 must not
        // stay in the macro word until a context change)
        if (!r.vk_rel_ms) r.vk_rel_ms = in->now_ms;
        if (totk_pcvk_frames() - r.vk_frame0 < 2 && in->now_ms - r.vk_rel_ms < 500) return true;
        totk_pcvk_macro_hold(r.held_vk, false);
        r.held_vk = 0;
        return false;
    }
    case MAC_ABILITY_1: case MAC_ABILITY_2: case MAC_ABILITY_3:
    case MAC_ABILITY_4: case MAC_ABILITY_5: case MAC_ABILITY_6: {
        const int n = r.id - MAC_ABILITY_1 + 1;
        // the wheel's own GameData commit, replayed on the UI thread (the totk_ability.cpp, G2); it checks that the
        // ability is unlocked and logs "[ability]..."
        const bool ok = totk_ability_request(n);
        static int logs;
        if (logs++ < 100) printf("[pc] ability %d: %s\n", n, ok ? "requested" : "not queued (no UI update now)");
        return false;
    }
    case MAC_NEXT: case MAC_PREV: case MAC_ARROW_NEXT: case MAC_ARROW_PREV: {
        // d-right held until the weapon strip is in front (the TMS_QUICKMENU; at most 20 frames), then one flick of
        // the right stick (2 frames), d-right 2 frames more, release: the game equips the next weapon. Aiming, the
        // same with d-up: the arrow's material strip (3.3; ZR stays MAC_BOW's)
        const bool arrow = r.id == MAC_ARROW_NEXT || r.id == MAC_ARROW_PREV;
        const uint32_t pad = arrow ? kDU : kDR;
        const int dir = r.id == MAC_NEXT || r.id == MAC_ARROW_NEXT ? 1 : -1;
        if (!r.step_at) {
#ifdef TOTK_TMS_QUICKMENU
            const bool open = TOTK_MENU_SCREEN(totk_menu_screen()) == TMS_QUICKMENU;
#else
            const bool open = d >= 6;
#endif
            if (!open && d < 20) { o->bits |= pad; return true; }
            r.step_at = f;
            static int logs;
            if (logs++ < 50) printf("[pc] %s step: the strip %s at frame +%llu\n", arrow ? "arrow" : "weapon",
                                    open ? "open" : "not seen", (unsigned long long)d);
        }
        const uint64_t e = f - r.step_at;
        if (e < 2) { o->bits |= pad; rstick(o, dir, 0); return true; }
        if (e < 4) { o->bits |= pad; return true; }
        // (the next weapon equipped, as the 1 strip's pick: LMB swings from here on, 4.23's latch)
        if (!arrow) totk_player_hand_note(TPH_MELEE);
        return false;
    }
    case MAC_BOW_STRIP: {
        // "3", held or kept open by the sticky strip (4.26: 3 only chooses). ZR + d-right held opens the bow strip: the
        // game opens the bow page on the UI controller's ZR hold + d-right, the aim is only a side effect of the pad's
        // ZR reaching the player, so Link draws nothing. The wheel steps the strip (the quick menu's feed). At the
        // close ZR goes up first and d-right stays 2 frames more (one game frame at least): a ZR still held when the
        // strip closes makes onOutBefore set the take-out-bow request (GameData 0x2f03cd3e) and Link raises the bow.
        // Then d-right up: the strip equips the one in front, nothing in hand changes, so there is no B + ZR lowering
        // any more. With LMB's draw already up, d-right only (ZR is the bow's) (the pad's ZR is hidden from the player,
        // VK 13, from the press until 2 frames after d-right goes up: the pad reaches the game a frame or so after the
        // router writes it)
        if (first) {
            r.strip_only = g_bow_held.load(std::memory_order_relaxed) > 0;
            if (!r.strip_only) { totk_pcvk_pad_mask(1ull << 13, true); r.vk13_masked = true; }
        }
        if (!r.released) {
            o->bits |= r.strip_only ? kDR : kDR | kZR;
            return true;
        }
        if (r.strip_only) return false;
        const uint64_t e = f - r.released_at;
        if (e < 2) { o->bits |= kDR; return true; }   // (ZR up, d-right still held: the strip closes after it)
        if (e < 4) return true;                        // (nothing held: the mask outlasts the pad's last ZR)
        totk_pcvk_pad_mask(1ull << 13, false);
        r.vk13_masked = false;
        return false;
    }
    case MAC_VEHICLE_LEAVE: {
        // Shift on a steering stick, the sled (RideSled 199) or the telescope (RideTelescopeBase 257): one tap of the
        // pad B's whole set (VK 1 | 5 | 25), at the press. It lets go of the stick (rc_veh2, trig 0x2000022) and gets
        // Link off each ride. E is the pad's A there now (VEHICLE "interact", VK 4 | 16: Yunobo's launch from a ride).
        // Off a ride, nothing
        if (first) {
            const int v = totk_player_verb(0);
            r.on_stick = totk_player_on_stick() || v == 199 || v == 257;   // (decided at the press)
        }
        if (!r.on_stick) return false;
        if (first) totk_pcvk_tap((1ull << 1) | (1ull << 5) | (1ull << 25));
        // (then nothing until Shift goes up, with Shift claimed: the let-go flips the context to world, where a still-
        // held Shift would sprint; a key still held when the claim ends stays dead until released)
        o->claim[0] = in->input;
        return !r.released;
    }
    case MAC_WEAPON_STRIP:
        // "1" held (the T kind starts this row at 250 ms; a shorter press is the T row's tap): d-right held opens the
        // weapon strip (the wheel steps it: the quickmenu feed); the release equips the one in front, often onto
        // Link's back with no verb of its own (owner1), so it sets the hand latch: LMB swings from here on
        if (!r.released) { o->bits |= kDR; return true; }
        totk_player_hand_note(TPH_MELEE);
        return false;
    case MAC_HOLSTER: {
        // "Q", the holster toggle. The melee weapon, the bow or the shield drawn alone in hand: one latched VK 5 tap, the
        // pad's B (put away, verb 58; the lone shield is ExecutePlayerDrawn's case 2). Empty hands with a melee weapon
        // equipped: one latched VK 9 tap, which draws it with no swing only while Link stands (slot-0 verb 40 Wait). While
        // he moves (verb 1 Walk) the game's VK 9 with the weapon sheathed is the walk scheduler's DashAttack, and the game
        // has no draw-only input. So the movement is held at 0 until verb 40, then the tap; held WASD resumes once the run
        // ends. Capped at kHolsterStopMs: past it, nothing. VK 5 held is sprint, so a one-frame tap only, and rows only in
        // WORLD / AIM. Decided at the press
        constexpr uint64_t kHolsterStopMs = 200;
        auto draw = [&](const char *what) {
            totk_pcvk_tap(1ull << 9);
            totk_player_hand_note(TPH_MELEE);
            static int logs;
            if (logs++ < 100) printf("[pc] holster: %s\n", what);
        };
        if (first) {
            const uint32_t st = totk_player_state();
            const char *what = "nothing (no player)";
            if (st & TPS_VALID) {
                if (st & (TPS_WEAPON_OUT | TPS_BOW_OUT | TPS_SHIELD_OUT)) {
                    totk_pcvk_tap(1ull << 5);
                    if (st & TPS_BOW_OUT) g_bow_asked_ms = 0;
                    what = "put away (VK 5 tap)";
                } else if (st & TPS_WEAPON_EQUIPPED) {
                    if (totk_player_verb(0) == 40) { draw("the weapon out (VK 9 tap)"); return false; }
                    r.q_stop = true;
                    r.q_at = in->now_ms;
                    what = "stopping to take the weapon out";
                } else {
                    what = "nothing (no melee weapon equipped)";
                }
            }
            static int logs;
            if (logs++ < 100) printf("[pc] holster: %s\n", what);
            if (!r.q_stop) return false;
        }
        // waiting for Link to stand: the left stick held at 0 (a macro's stick goes over keys and pads)
        o->stick[0] = true;
        o->sx[0] = o->sy[0] = 0;
        if (totk_player_verb(0) == 40) {
            draw("the weapon out after a stop (VK 9 tap)");
            return false;
        }
        if (in->now_ms - r.q_at >= kHolsterStopMs) {
            static int logs;
            if (logs++ < 100) printf("[pc] holster: gave up (Link did not stand within %llu ms)\n",
                                     (unsigned long long)kHolsterStopMs);
            return false;
        }
        return true;
    }
    case MAC_BOW_OUT:
    case MAC_BOW_TOGGLE: {
        // "F", the bow toggle (4.26: "para sacar el arco con la F"). The bow in hand (TPS_BOW_OUT): one latched VK 5 tap,
        // the pad's B, which puts it away (verb 58, as 4.23's holster). Else the game's own take-out, the bow strip's
        // request (the totk_player_request_bow: UseBow 59 with no ZR, so no aim and no arrow), from a drawn sword too. It
        // refuses with no bow equipped, in an event, on a vehicle or in an ability: F does nothing (on a horse it takes
        // the bow out since the 42449fc6: the take-out test has a Ride case). F never puts away the sword or a shield
        // (Shift does, MAC_SPRINT). WORLD / AIM; MAC_BOW_OUT (AIR / GLIDE, and HORSE) is the take-out alone: the VK 5 tap
        // closes the glider (ExecutePlayerGlide) and is read by ExecutePlayerRide (the get-off), so with the bow in hand
        // it does nothing. Decided at the press
        if (!first) return false;
        const uint32_t st = totk_player_state();
        const char *what = "nothing (no player)";
        if (st & TPS_VALID) {
            if ((st & TPS_BOW_OUT) && r.id == MAC_BOW_OUT) {
                what = "nothing (the bow is in hand; its put-away would close the glider / leave the saddle)";
            } else if (st & TPS_BOW_OUT) {
                totk_pcvk_tap(1ull << 5);
                g_bow_asked_ms = 0;
                what = "the bow put away (VK 5 tap)";
            } else if (totk_player_request_bow()) {
                g_bow_asked_ms = in->now_ms ? in->now_ms : 1;   // (LMB draws until the bow shows, kBowSoonMs at most)
                what = "the bow taken out (the game's request)";
            } else {
                g_bow_asked_ms = 0;
                what = st & TPS_BOW_EQUIPPED ? "nothing (not now: an event, a vehicle or an ability)"
                                             : "nothing (no bow equipped)";
            }
        }
        static int logs;
        if (logs++ < 100) printf("[pc] bow toggle: %s\n", what);
        return false;
    }
    case MAC_SPRINT: {
        // Shift (4.25). The pad's B does both: its trigger puts away what is in hand (VK 5, the holster's), its hold
        // sprints. So, at a press on foot with the weapon, the bow or the shield alone in hand (TPS_WEAPON_OUT /
        // TPS_BOW_OUT / TPS_SHIELD_OUT, the hands_state: ExecutePlayerDrawn's case 2 is the shield drawn alone, and B puts
        // that away too), one latched VK 5 tap, at the press only (never with nothing in hand: VK 5 is the back bit, a
        // stray one cancels); then VK 1 held in the hook's own word for as long as Shift is down (as MAC_ATTACK's VK 9),
        // or, before the hook has run once, the pad's B held, as the G row it replaced. The rows of WORLD, AIR and AIM are
        // this one macro, so the run carries a held Shift through a jump or out of the aim (the key goes dead at a context
        // change onto an M row; the run carries, as LMB's attack). A press in the air or while aiming only sprints (VK 5
        // is the cancel / the glider elsewhere)
        if (first) {
            const uint32_t st = totk_player_state();
            // carrying (verb 44 Carry in any slot): the carry's "B Quit" (drawn as Shift) reads a VK 5 trigger only
            // (ExecutePlayerCarry 0x1001cf1764, so the press taps it too, as the pad's B does there
            bool carrying = false;
            for (int s = 0; s < 5; s++) carrying = carrying || totk_player_verb(s) == 44;
            const bool put_away = in->ctx == PCX_WORLD && (st & TPS_VALID) &&
                                  ((st & (TPS_WEAPON_OUT | TPS_BOW_OUT | TPS_SHIELD_OUT)) || carrying);
            if (put_away) totk_pcvk_tap(1ull << 5);
            // (plus a latched VK 1 tap, as the G row's press did: a Shift shorter than a game frame still reaches the
            // game MAC_ATTACK does the same for VK 9)
            if (totk_pcvk_frames() > 0) {
                totk_pcvk_tap(1ull << 1);
                totk_pcvk_macro_hold(1ull << 1, true);
                r.held_vk = 1ull << 1;
            }
            static int logs;
            if (logs++ < 100)
                printf("[pc] sprint: %s%s\n", put_away ? "put away (VK 5 tap), then " : "",
                       r.held_vk ? "VK 1 held" : "the pad's B held (no VK hook yet)");
        }
        if (r.released) {
            if (r.held_vk) totk_pcvk_macro_hold(r.held_vk, false);
            r.held_vk = 0;
            return false;
        }
        if (!r.held_vk) o->bits |= kB;
        return true;
    }
    case MAC_PULL: case MAC_PUSH:
        if (d < 6) { o->bits |= r.id == MAC_PULL ? kDU : kDD; return true; }
        return false;
    case MAC_ROTATE: {
        if (!r.released) { count_steps(r, in->mouse_dx, in->mouse_dy, true); o->take_mouse = true; }
        int sx, sy;
        const bool busy = step(r, f, &sx, &sy);
        if (sx || sy) o->bits |= sx > 0 ? kDR : sx < 0 ? kDL : sy > 0 ? kDU : kDD;
        if (!r.released || busy) { o->bits |= kR; return true; }
        return false;
    }
    case MAC_RAISE: case MAC_LOWER:
        if (r.released) return false;
        rstick(o, 0, r.id == MAC_RAISE ? 1 : -1);
        return true;
    case MAC_UNSTICK:
        if (d >= 24) return false;
        rstick(o, (d / 2) % 2 ? 1 : -1, 0);
        return true;
    case MAC_SECONDARY: {
        const uint8_t a = pc_ctx_ability();
        // Ultrahand releases only what it holds (A is its grab / release toggle: no grab from RMB)
        const bool holding = (totk_player_state() & TPS_HOLDING) != 0;
        const uint64_t bits = a == PCA_RECALL ? kL : a == PCA_FUSE ? kZL : a == PCA_ULTRAHAND ? (holding ? kA : 0)
                            : a == PCA_AUTOBUILD ? kA : 0;
        if (d < 2 && bits) { o->bits |= bits; return true; }
        return false;
    }
    case MAC_PRIMARY: {
        const uint64_t bits = pc_ctx_ability() == PCA_FUSE ? kY : kA;
        if (d < 2) { o->bits |= bits; return true; }
        return false;
    }
    case MAC_CONSTRUCT_ATTACH: {
        // "E" on Mineru's construct. Its inputs only raise the robot's flags: VK 26 (the pad's X, "Attach") 0x2000; the
        // parts' VK 12 / 6 / 9 (R / L / Y) a trigger flag (0x400 / 0x800 / 0x1000) and, held, a hold flag (0x8000000 /
        // 0x10000000 / 0x20000000), which fires that part's device. In the attach the same L / R / Y choose the part, so a
        // part key held through the pick kept firing the device once the attach closed. The runtime cannot see the guide's
        // mode (no guide-type read), so the press opens a window (kAttachMs; a pick, a cancel or another context closes
        // it): the run claims the part keys and Esc. A part key's press is one latched tap of its VK (the pick: trigger and
        // hold for one game frame, nothing after), and the key stays dead until let go (the claim's rule) Esc is the
        // guide's "B Cancel" (the pad's B 2 frames), not the inventory. Space (MAC_CONSTRUCT_LEAVE) cancels in the window
        // too. Near no device the X is the pad's own; the window then only turns the next part press into a one-frame tap.
        // With the attach-screen read (TOTK_HAS_ATTACH_SCREEN: AttachTargetSelectCommand_00 opened, the screen the attach
        // mode opens) the window is the screen itself: after E, up to kAttachOpenMs for it to open (else the X was the
        // construct's jump: no claim, nothing changes), then the claim and the picks for as long as it stays open; the
        // timed window only when that read has no live UI hook (-1). With the guard held (VK 2 in the hook's word: Shift) E
        // is the pad's A instead, one VK 4 tap and no attach: the robot reads VK 4's trigger (0x80000000) and on the Switch
        // A while guarding pushes the arms out (the parry; on foot RMB + E is the same chord, 3.10
        static int logs;
        if (first && (totk_pcvk_held() & (1ull << 2))) {
            totk_pcvk_tap(1ull << 4);
            if (logs++ < 100) printf("[pc] construct: guard + E: the parry (VK 4 tap), no attach\n");
            return false;
        }
        // The read is the construct AI's own IsFoundAttachObj; unreadable (-1): the X as before (attach near a device,
        // else the construct's jump)
#ifdef TOTK_HAS_CONSTRUCT_REACH
        if (first && totk_construct_attach_in_reach() == 0) {
            totk_pcvk_tap(1ull << 4);
            if (logs++ < 100) printf("[pc] construct: E with no device in reach: the do-action (VK 4 tap)\n");
            return false;
        }
#endif
        struct Part { const char *action; int dflt, vk; };
        static const Part kParts[] = {{"right_arm", kVkLButton, 12}, {"left_arm", kVkRButton, 6}, {"back_part", kVkQ, 9}};
        int keys[4] = {}, vks[3] = {}, n = 0;
        for (const Part &p : kParts) {
            int k[4];
            if (add_keys(PCX_CONSTRUCT, p.action, p.dflt, in->input, k, 0) > 0) { keys[n] = k[0]; vks[n] = p.vk; n++; }
        }
        int esc[4];
        const int ne = add_keys(PCX_CONSTRUCT, "pause", kVkEscape, in->input, esc, 0);
        const int nk = n + (ne > 0 ? 1 : 0);
        if (ne > 0) keys[n] = esc[0];
        unsigned down = 0;
        for (int i = 0; i < nk; i++) if (in->down(keys[i], in->user)) down |= 1u << i;
#ifdef TOTK_HAS_ATTACH_SCREEN
        const int screen = totk_attach_screen_open();   // (1 open, 0 not, -1 no live UI hook: the timed window)
#else
        const int screen = -1;
#endif
        if (first) {
            totk_pcvk_tap(1ull << 26);
            g_attach_until_ms = in->now_ms + (screen >= 0 ? kAttachOpenMs : kAttachMs);
            r.cx_was = down;   // (a key already down when E went down is no pick)
            r.cx_ms = in->now_ms;
            if (logs++ < 100)
                printf("[pc] construct: attach (VK 26 tap), %s\n",
                       screen >= 0 ? "waiting for the attach screen" : "the part keys pick for a while (no screen read)");
            if (screen < 0) for (int i = 0; i < nk; i++) o->claim[i] = keys[i];
            return true;
        }
        if (r.cx_end) {   // the cancel's B
            for (int i = 0; i < nk; i++) o->claim[i] = keys[i];
            if (f < r.cx_end) { o->bits |= kB; return true; }
            return false;
        }
        const unsigned pressed = down & ~r.cx_was;
        r.cx_was = down;
        if (screen >= 0) {   // the screen decides
            if (screen == 1) {
                r.cx_seen = true;
                g_attach_until_ms = in->now_ms + kAttachMs;   // (MAC_CONSTRUCT_LEAVE reads it: Space is the cancel)
            } else if (r.cx_seen || in->now_ms - r.cx_ms >= kAttachOpenMs) {
                g_attach_until_ms = 0;
                if (logs++ < 100)
                    printf("[pc] construct: %s\n", r.cx_seen ? "the attach screen closed" : "no attach screen (the X was a jump)");
                return false;
            } else {
                return true;   // (not open yet: no claim, the keys keep their rows)
            }
        } else if (!g_attach_until_ms || in->now_ms >= g_attach_until_ms) {
            g_attach_until_ms = 0;
            if (logs++ < 100) printf("[pc] construct: the attach window closed\n");
            return false;
        }
        for (int i = 0; i < nk; i++) o->claim[i] = keys[i];
        for (int i = 0; i < n; i++)
            if (pressed >> i & 1) {
                totk_pcvk_tap(1ull << vks[i]);
                g_attach_until_ms = 0;
                if (logs++ < 100) printf("[pc] construct: %s picked (one VK %d tap, no hold)\n", kParts[i].action, vks[i]);
                return false;
            }
        if (ne > 0 && (pressed >> n & 1)) {
            g_attach_until_ms = 0;
            r.cx_end = f + 2;
            o->bits |= kB;
            if (logs++ < 100) printf("[pc] construct: attach cancelled (the pad's B)\n");
            return true;
        }
        return true;
    }
    case MAC_CONSTRUCT_LEAVE: {
        // "Space" on the construct: get off. One clean B: its VK 25 trigger takes ExecutePlayerRideRobot from riding to
        // GolemPreGetOff (state 3), which requests the get-off itself every frame and the dismount runs on its own. So
        // the pad's B 2 frames, nothing after (25e04334's A 250 ms later is gone: VK 4 is the construct's do-action,
        // mIsTriggerDoAction). The game refuses that B during the attach, where it is the attach's "B Cancel" (the same
        // B; the attach window closes). The game draws no get-off prompt
        static int logs;
        if (first) {
            const bool cancel = g_attach_until_ms && in->now_ms < g_attach_until_ms;
            g_attach_until_ms = 0;
            r.cx_end = f + 2;
            if (logs++ < 100) printf("[pc] construct: %s\n", cancel ? "attach cancelled (the pad's B)" : "get off (the pad's B)");
        }
        if (f < r.cx_end) { o->bits |= kB; return true; }
        return false;
    }
    case MAC_RECIPE_HISTORY:
    case MAC_RECIPE_OPEN: {
        // "R" in the pause menu: on the Food tab (the pause sub 5) the pad's X 2 frames, the tab's "recipe book" button;
        // elsewhere nothing (X is another tab's action there)
        static int logs;
        if (first) {
            const int sub = pc_ui_sub();
            if (sub == 5) r.cx_end = f + 2;
            const bool hist = r.id == MAC_RECIPE_HISTORY;
            if (logs++ < 100)
                printf("[pc] recipe book: %s %s the Food tab: %s\n", hist ? "H" : "R", sub == 5 ? "on" : "off",
                       sub != 5 ? "nothing" : hist ? "the pad's Y (View history)" : "the pad's X");
        }
        // (MAC_RECIPE_HISTORY: H, the Food tab's "View history", the pad's Y there)
        if (f < r.cx_end) { o->bits |= r.id == MAC_RECIPE_HISTORY ? kY : kX; return true; }
        return false;
    }
    default:
        return false;
    }
}

void drop(Run &r) {
    if (r.held) g_bow_held.fetch_sub(1, std::memory_order_relaxed);   // (a bow run cut while drawn)
    if (r.held_vk) totk_pcvk_macro_hold(r.held_vk, false);              // (a held VK run cut while down)
    if (r.vk13_masked) totk_pcvk_pad_mask(1ull << 13, false);           // (a bow strip cut while open)
    if (r.id == MAC_CONSTRUCT_ATTACH) g_attach_until_ms = 0;            // (the attach window cut: another context)
    r = Run();
}
void abort_run(int input) { if (Run *r = run_of(input, false)) drop(*r); }
// One ops table per id (the router passes no id to begin): each begin starts its own, replacing the key's run
template <int Id> void begin_id(int, int input) {
    Run *r = run_of(input, true);
    drop(*r);
    r->id = Id;
    r->input = input;
}
template <int Id> constexpr PcMacroOps ops_for() { return PcMacroOps{begin_id<Id>, tick, abort_run}; }
template <int... Ids> struct OpsTable {
    static constexpr PcMacroOps ops[] = {ops_for<Ids>()...};
    static constexpr int ids[] = {Ids...};
};
using Table = OpsTable<MAC_DODGE, MAC_PARRY, MAC_BOW, MAC_ABILITY_1, MAC_ABILITY_2, MAC_ABILITY_3, MAC_ABILITY_4,
                       MAC_ABILITY_5, MAC_ABILITY_6, MAC_NEXT, MAC_PREV,
                       MAC_PULL, MAC_PUSH, MAC_ROTATE, MAC_RAISE, MAC_LOWER, MAC_UNSTICK, MAC_SECONDARY, MAC_PRIMARY,
                       MAC_ARROW_NEXT, MAC_ARROW_PREV, MAC_BOW_STRIP, MAC_VEHICLE_LEAVE, MAC_ATTACK, MAC_WEAPON_STRIP, MAC_HOLSTER, MAC_BOW_TOGGLE, MAC_SPRINT,
                       MAC_BOW_OUT, MAC_BOW_DRAW, MAC_CONSTRUCT_ATTACH, MAC_CONSTRUCT_LEAVE, MAC_RECIPE_OPEN,
                       MAC_RECIPE_HISTORY>;
static_assert(sizeof(Table::ids) / sizeof(int) == MAC_COUNT - 1, "every macro id has its ops");

bool register_all() {
    pc_ctx_player_map_register(from_player);
    for (size_t i = 0; i < sizeof(Table::ids) / sizeof(int); i++) pc_macro_register(Table::ids[i], &Table::ops[i]);
    pc_vbtn_writer_register(totk_pcvk_hold, totk_pcvk_tap);
    totk_pcvk_set_active(true);   // (the router calls hold / tap only for G rows, and none under the console layout)
    return true;
}
[[maybe_unused]] const bool g_registered = register_all();

}  // namespace
