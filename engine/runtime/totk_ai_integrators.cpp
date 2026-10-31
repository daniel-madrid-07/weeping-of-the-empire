// totk_ai_integrators.cpp: AI-program values that the game integrates once per AI update keep the Switch's rate at
// any frame rate.
//
// Some AINB programs integrate a blackboard value with a setter node that re-runs every update (ExecuteBlackboardSet* with
// ExecuteOnce = false) whose input reads the same key through math with no dt: wildlife caution decays by 1 per update
// (QuadrupedCommon.utility N3, BattleAnimalCommon.utility N15: three noises within 350 updates make an animal flee, 11.7 s at 30 fps
// but 1.5 s at 240, so animals barely spook), the Soldier Construct's dash speed lerps 20% per update
// (Zonau_Golem.action.dashattackcommon N16), the Lynel's retreat tail counts 10 updates, the Molduga's swim speed chases 10% per
// update, Colgera's rush adds its acceleration per update, the shared Rush lerp, the Demon Dragon's glow ramp, and the ramp list
// (water-plane timer, fan spin-up, Keese / seal / bird speeds, the light switches' charge / hold timers). They all run through
// ExecuteBlackboardSetToBase::updateImpl_ (main+0x2833b60, x0 = the node, x1 = the AI update context: +8 its step in 30 Hz frames),
// shared by every blackboard setter.
//
// The hook identifies a node from x0 alone (node +8 -> ai::Context, +0x10 -> the file's ai::Extractor, one per loaded
// AINB: +0x10 the file; node +0x1c = its index; the file's name at file + s32[+0x24] + s32[+0x08]), checks it against
// the allowlist below (per-thread cache keyed by the Extractor, re-checked against the file pointer and the name's
// first bytes), and for an allowlisted node sums the update's step per node: the original setter runs once per whole
// 30 Hz frame of the actor's AI time and is skipped in between, so the value steps as on the Switch at 30 fps (an
// actor whose AI updates every few frames gets those frames' step). A listed setter not called for 50 ms (more than a
// 30 Hz frame: below 30 fps the hook is not in play) runs at once and restarts its sum, as its first update would at
// 30 fps: a per-update setter resuming after its state was left, or a Set that runs once per state entry, is never
// skipped. Every other setter, and every call at 30 fps and below (step >= 1), is the original exactly. gen.py --hook
// 2833b60 in GEN_MAIN; TOTK_AI_INTEGRATORS=0: the original alone. tools/oracle/test_ai_integrators.py.
//
// A value that is DRAWN (a look target the head follows) must not step at 30 Hz, so its easer is scaled instead
// (kScale): the calcs of QueryMathChaseF32ToF32 (main+0x200f61c), QueryMathChaseVec3fToVec3f (0x200f8b0) and
// QueryMathLerpF32 (0x20147f4) are the query vslot 0x110 (x0 = the node) and read their Speed / Rate through the tagged
// pointer at node +0x30 (low 2 bits flags). Their bodies never read x1, but the same slot of the step-aware
// QueryMathApproachVector3 (0x200a8c0) loads its step from [x1 + 8] at entry (0x200a8dc) with no check, so the query
// dispatcher passes the update context in x1 for every query calc; the hook still reads it only for a listed node. For
// a listed easer node the hook points +0x30 at a stack copy of Speed x step (a chase) or 1 - (1 - Rate)^step (a lerp),
// runs the original and puts the pointer back, so the value moves every frame at the Switch's rate per second. Those
// classes ignore the step themselves, which is why they are frame-rate dependent. The same swap scales a constant
// per-update increment: QueryMathAddF32's calc (0x27dcb98) adds InputA (tagged pointer at +0x28) and InputB (+0x30), so
// a listed adder's constant input becomes constant x step (kScaleAddA / kScaleAddB): the water Octorok's bob angle (+6
// per update) and the ice Like Like's sprinkler sweep (+45 per update). gen.py --hook 200f61c,200f8b0,20147f4,27dcb98.
// Every listed node is also prefiltered by its index (a 256-bit mask per table), so an unlisted setter / calc pays one
// load and a bit test.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace {
struct Allowed { const char *file; unsigned short nodes[12]; };   // node indices, 0xffff-terminated
constexpr Allowed kAllow[] = {
    {"QuadrupedCommon.utility.module", {3, 0xffff}},
    {"BattleAnimalCommon.utility.module", {15, 0xffff}},
    {"Zonau_Golem.action.dashattackcommon.module", {16, 0xffff}},
    {"Lynel_EscapeFromTarget.actionseq.root", {7, 0xffff}},
    {"Lynel_UnidentifedTerrorDiscoverd.actionseq.root", {7, 0xffff}},
    {"Sandworm.action.swim.module", {33, 0xffff}},
    {"DungeonBossRito.action.RushAttack.module", {16, 18, 93, 94, 95, 96, 152, 153, 154, 155, 156, 0xffff}},
    {"Rush.actionseq.root", {28, 29, 0xffff}},
    {"Enemy_Dragon_Darkness.root", {133, 0xffff}},
    // per-update ramps: a QueryMathChaseF32ToF32 / LerpF32 / ChaseVec3fToVec3f easer, whose calc ignores the update
    // step, feeding a setter of the key it reads (node indices = ai::Element.mIndex). Easers whose calc already scales
    // by the step (ApproachVector3, EaseInApproachPosition, ApproachF32, AddCalc,...) must NOT be listed: they would
    // run slower than on the Switch.
    {"FldObj_WaterPlane_A_03.root", {7, 8, 0xffff}},                  // WaterStorageCounter: the 60 s fill / drain
    {"WindGenerator.root", {6, 0xffff}},                              // RotSpeedRatio spin-up, 0.0333 per update
    {"PropellerControl.module", {12, 0xffff}},                        // RotSpeedRatio spin-up (DgnObj_PropellerAndWind)
    {"_LocalModule_128f9e531327.module", {17, 0xffff}},               // Keese RiseSpeed
    {"KeeseSwarm.action.dashup.module", {24, 0xffff}},                // Keese swarm AscendSpeed
    {"Sunazarashi.action.flee.module", {31, 0xffff}},                 // sand seal flee Speed
    {"Bird.action.freemovetotarget.module", {9, 0xffff}},             // gull / hawk ForwardSpeed
    {"Assassin_Junior.action.basicattack.module", {21, 0xffff}},      // Yiga lunge RotSpdMax
    {"Ganondorf_Miasma_CounterBullet.root", {12, 0xffff}},            // Velocity chase
    {"Ganondorf_Miasma_RitoMiasma.root", {17, 0xffff}},               // Velocity chase
    // Lightning Temple / Gerudo underground light switches: ChargingRate += 1 / ChargingFrame (30) per update while lit,
    // FullChargeTimer -= 1 / FullChargeFrame (75: the 2.5 s the switch holds after the light leaves), ChargingRate -=
    // 1 / DischargeFrame (30). At 144 fps the hold lasts 0.52 s.
    {"DgnObj_Gerudo_LightSwith_A_01.root", {6, 17, 24, 0xffff}},
    {"PyramidObj_GerudoUndergroundStatue_A_01_LightSwitch.root", {6, 17, 24, 0xffff}},
    // Only easers whose calc ignores the step: LerpF32, ChaseF32ToF32, ChaseVec3fToVec3f, plain Mul / Add math. (the Like
    // Likes' LastLookPos is drawn, the head aims at it: its easers are in kScale. Octorok Notice's RotSpeedRadPerFrame
    // decay is not traced to every update: out)
    {"Octarock_Snow.action.Strafe.module", {44, 0xffff}},             // ForwardSpeedMpf chase
    {"Zonau_Golem.action.backstep.module", {19, 0xffff}},             // Construct backstep HorizontalSpeed lerp
    {"Sunazarashi.action.swim.module", {36, 0xffff}},                 // sand seal CurrentMaxSpeed lerp 0.5
    {"Obj_ZonauContainer_A.root", {63, 0xffff}},                      // ContainerResponseRate lerp
    // (out: the Flux Construct modules _LocalModule_339eaec38ec1 / _764c8d42309e. Every listed setter there, the rotation
    // lerps and RollingSpeedRate, sits in a SplitTiming Enter branch (the ainbtree.py): once per state entry, an event,
    // not a per-update ramp)
    {"_LocalModule_cba9d3178d5b.module", {53, 0xffff}},               // LerpedMaxRotateSpeedDpf
    // EXB expressions that step their own key each update (the exb.py decode, gp-ainb-integrators.md "X").
    {"NpcSage.action.BasicAttack.module", {11, 0xffff}},  // sage RotRadRatio = min(+ 0.05, 1)
    {"BirdCommon.utility.module", {1, 0xffff}},           // FramesStuckOnTerrain + 1 while stuck
    // QueryApproachSpeed (0x1001f0fc6c) is step-free; its InitSpeed is the body's forward speed, which tracks the last
    // Speed written, so a throttled setter ramps once per 30 Hz step
    {"Lynel.action.dashattack.module", {56, 0xffff}},     // Speed: 20% of the gap to the dash speed per update
    {"Lynel.action.rushattack.module", {40, 0xffff}},     // Speed: 5% per update
};
// easer nodes (Chase / Lerp) scaled by the step, for drawn values
constexpr Allowed kScale[] = {
    {"LikeLike.action.chemicalattack.module", {59, 0xffff}},   // ChaseVec3f -> LastLookPos: the head's aim
    {"LikeLike.action.electricattack.module", {64, 0xffff}},
    {"LikeLike.action.iceattack.module", {14, 0xffff}},
    {"LikeLike.action.rockattack.module", {50, 0xffff}},
    {"LikeLike.alwaysRockAction.module", {35, 0xffff}},
};
// QueryMathAddF32 nodes whose constant InputA (kScaleAddA) / InputB (kScaleAddB) is scaled by the step: a drawn value
// stepped by a constant each update ([Update] setters of the same key, gp-ainb-integrators.tsv G rows)
constexpr Allowed kScaleAddA[] = {
    {"LikeLike.action.sprinklerattack.module", {5, 0xffff}},   // ang = 45 + ang -> [4]: the ice Like Like's spray sweep
};
constexpr Allowed kScaleAddB[] = {
    {"Octarock_Water.action.Levitate.module", {4, 0xffff}},    // CurAngle + 6 -> [3]: the water Octorok's bob
};
constexpr int kNumAllow = sizeof kAllow / sizeof kAllow[0], kNumScale = sizeof kScale / sizeof kScale[0];
constexpr int kNumAddA = sizeof kScaleAddA / sizeof kScaleAddA[0], kNumAddB = sizeof kScaleAddB / sizeof kScaleAddB[0];

template <const Allowed *T, int N>
struct IdxMask {   // the node indices a table lists (any index >= 256: no prefilter)
    u64 w[4] = {};
    bool all = false;
    constexpr IdxMask() {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < 12 && T[i].nodes[j] != 0xffff; j++) {
                if (T[i].nodes[j] >= 256) all = true;
                else w[T[i].nodes[j] >> 6] |= 1ull << (T[i].nodes[j] & 63);
            }
    }
};
template <const Allowed *T, int N>
constexpr IdxMask<T, N> kMask{};

struct Seen { u64 ext, file, head; int allow; };   // allow: index into the table, -1 = not listed

template <const Allowed *T, int N>
int file_entry(u64 ext) {   // the table entry of this Extractor's file, or -1
    static thread_local Seen t_seen[256];
    const u64 file = ld64(ext + 0x10);
    Seen &s = t_seen[(ext >> 4) & 255];
    // (a hit is re-checked against the file pointer and, for a listed file, the whole name: an Extractor freed and
    // reallocated for another file at the same address is caught, also when the names share a prefix)
    if (s.ext == ext && s.file == file && (s.allow < 0 || !strncmp((const char *)s.head, T[s.allow].file, 96)))
        return s.allow;
    s = {ext, file, 0, -1};
    if (!totk_ptr_ok(file)) return -1;
    const u64 name = file + (u64)(s64)(s32)ld32(file + 0x24) + (u64)(s64)(s32)ld32(file + 0x08);
    if (!totk_ptr_ok(name)) return -1;
    s.head = name;
    for (int i = 0; i < N; i++)
        if (!strncmp((const char *)name, T[i].file, 96)) { s.allow = i; break; }
    return s.allow;
}

template <const Allowed *T, int N>
bool listed(u64 node) {   // node (x0 of a setter / calc) is in table T
    const unsigned i0 = ld16(node + 0x1c);
    if (!kMask<T, N>.all && (i0 >= 256 || !((kMask<T, N>.w[i0 >> 6] >> (i0 & 63)) & 1))) return false;
    const u64 actx = ld64(node + 8);
    const u64 ext = totk_ptr_ok(actx) ? ld64(actx + 0x10) : 0;
    const int a = totk_ptr_ok(ext) ? file_entry<T, N>(ext) : -1;
    if (a < 0) return false;
    const unsigned idx = ld16(node + 0x1c);
    for (int i = 0; i < 12 && T[a].nodes[i] != 0xffff; i++)
        if (T[a].nodes[i] == idx) return true;
    return false;
}

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("TOTK_AI_INTEGRATORS");
        const bool v = !e || strcmp(e, "0");
        printf("[ai-integrators] listed AI-program integrators %s\n", v ? "step per 30 Hz of AI time, listed drawn easers scaled by the step" : "step per update, as the game (TOTK_AI_INTEGRATORS=0)");
        return v;
    }();
    return on;
}

struct Acc { u64 node, t; float acc; };   // t: the last call, ns
Acc g_acc[1024];   // direct-mapped by the node's address (a collision restarts that node's sum: at most one extra step)
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
constexpr u64 kGapNs = 50000000;   // a listed node not called for this long restarts (its first update runs)

u64 steady_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

u64 (*totk_aiint_clock)() = steady_ns;   // (tools/oracle/ai_integrators_test.cpp drives its own)

#ifdef _WIN32
void f_2833b60_orig(Ctx *__restrict c);
void f_200f61c_orig(Ctx *__restrict c);
void f_200f8b0_orig(Ctx *__restrict c);
void f_20147f4_orig(Ctx *__restrict c);
void f_27dcb98_orig(Ctx *__restrict c);
void aiint_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_2833b60_orig@@YAXPEIAUCtx@@@Z=?aiint_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_200f61c_orig@@YAXPEIAUCtx@@@Z=?aiint_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_200f8b0_orig@@YAXPEIAUCtx@@@Z=?aiint_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_20147f4_orig@@YAXPEIAUCtx@@@Z=?aiint_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_27dcb98_orig@@YAXPEIAUCtx@@@Z=?aiint_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_2833b60_orig(Ctx *__restrict c);
__attribute__((weak)) void f_200f61c_orig(Ctx *__restrict c);
__attribute__((weak)) void f_200f8b0_orig(Ctx *__restrict c);
__attribute__((weak)) void f_20147f4_orig(Ctx *__restrict c);
__attribute__((weak)) void f_27dcb98_orig(Ctx *__restrict c);
#endif
void hook_f_2833b60(Ctx *__restrict c) {   // ExecuteBlackboardSetToBase::updateImpl_(this, ctx)
#ifndef _WIN32
    if (!f_2833b60_orig) return;
#endif
    const u64 node = c->x[0], uctx = c->x[1];
    const float step = enabled() && totk_ptr_ok(uctx) ? ldf32(uctx + 8) : 1.0f;
    if (!(step > 0.01f && step < 1.0f) || !totk_ptr_ok(node) || !listed<kAllow, kNumAllow>(node)) {
        f_2833b60_orig(c);
        return;
    }
    bool run;
    const u64 now = totk_aiint_clock();
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Acc *p = &g_acc[(node >> 4) & 1023];
    if (p->node != node || (s64)(now - p->t) > (s64)kGapNs) *p = {node, now, 1.0f - step};   // (a new or resumed node steps on its first update)
    p->t = now;
    p->acc += step;
    if ((run = p->acc >= 1.0f - 1e-4f)) p->acc -= 1.0f;
    g_lock.clear(std::memory_order_release);
    if (run) f_2833b60_orig(c);
}

namespace {
// a node of table T: its input behind the tagged pointer at node + slot (a chase's Speed, a lerp's Rate, an adder's
// constant) seen through a scaled stack copy for this one calc. False: not listed, the caller runs the original.
template <const Allowed *T, int N>
bool scaled_calc(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 slot, bool lerp) {
    const u64 node = c->x[0], uctx = c->x[1];
    if (!enabled() || !totk_ptr_ok(node) || !listed<T, N>(node) || !totk_ptr_ok(uctx)) return false;
    const float step = ldf32(uctx + 8);   // (x1 read only for a listed node)
    const u64 tp = ld64(node + slot), sp = tp & ~3ull;
    if (!(step > 0.01f && step < 1.0f) || !totk_ptr_ok(sp & ~7ull)) return false;   // (a float may sit at 4 mod 8)
    const float v = ldf32(sp);
    alignas(8) volatile float k = lerp && v > 0.0f && v < 1.0f ? 1.0f - powf(1.0f - v, step) : v * step;
    st64(node + slot, (u64)(uintptr_t)&k | (tp & 3));
    orig(c);
    st64(node + slot, tp);
    return true;
}
}  // namespace

void hook_f_200f61c(Ctx *__restrict c) {   // QueryMathChaseF32ToF32 calc
    if (!scaled_calc<kScale, kNumScale>(c, f_200f61c_orig, 0x30, false)) f_200f61c_orig(c);
}
void hook_f_200f8b0(Ctx *__restrict c) {   // QueryMathChaseVec3fToVec3f calc
    if (!scaled_calc<kScale, kNumScale>(c, f_200f8b0_orig, 0x30, false)) f_200f8b0_orig(c);
}
void hook_f_20147f4(Ctx *__restrict c) {   // QueryMathLerpF32 calc
    if (!scaled_calc<kScale, kNumScale>(c, f_20147f4_orig, 0x30, true)) f_20147f4_orig(c);
}
void hook_f_27dcb98(Ctx *__restrict c) {   // QueryMathAddF32 calc: InputA +0x28, InputB +0x30
    if (!scaled_calc<kScaleAddA, kNumAddA>(c, f_27dcb98_orig, 0x28, false) &&
        !scaled_calc<kScaleAddB, kNumAddB>(c, f_27dcb98_orig, 0x30, false))
        f_27dcb98_orig(c);
}
