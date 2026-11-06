// cpu_place.h: where the game's threads run on any CPU layout (cpu_place.cpp). Windows CPU Sets, never fixed core
// numbers or hard affinity masks: the scheduler keeps the choice soft, and a core that does not exist is never named.
//   hot     the guest's threads, the render / present / audio / vsync / input threads: every thread by default (the
//           process default CPU Sets), so a thread nobody tagged is hot. On a hybrid CPU (Intel 12th gen+, Core Ultra)
//           the performance cores; on a CPU with several last-level caches (AMD dual-CCD) one of them, the one with the
//           most L3 (the 3D V-Cache CCD) or else the first.
//   any     pools a frame waits on (the pipeline workers and shader translators: "draws wait for them"; the ASTC /
//           upload workers the render thread joins): every logical processor, so a burst of compiles uses the
//           E-cores / the other CCD as well as the fast ones (on an E-core only, a held frame would be held longer).
//   helper background pools nothing waits on (pipeline precompile, the program / pipeline cache writers, file preopen):
//   the efficiency cores on a hybrid CPU, the other CCD(s) on a dual-CCD one (off the V-Cache). render the render thread
//   (exec.c async_main) alone on one physical core: both of its logical processors are kept out of every other set, so no
//   thread of ours runs on its SMT sibling. The last SMT core of hot (of the fastest class where hot is unrestricted),
//   when that class has >= 4 SMT cores (6 until v0.1.8: on a 4c/8t the render thread is the cap and the game's threads are
//   mostly blocked); none without SMT (no sibling) or with fewer. LARECOMP_RENDER_PIN=0: none (=auto: the default); =<n>:
//   the core holding logical processor n instead; also none when there is no render thread (exec.c backend_async_on:
//   LARECOMP_ASYNC_EXEC=0, a capture).
// A CPU with one kind of core and one L3 (the 9800X3D, most 6 / 8-core parts) places nothing else.
// TOTK_CPU_PLACE=0: no hot / helper split (A/B); the plan is logged at start either way ("[cpu] ...").
//   guest TOTK_CORE_MODEL=switch only: the Switch's 3 guest cores as
//           3 logical processors of their own, one on each of three physical cores of hot (fastest class first) other
//           than the render core's, out of every other set (their SMT siblings too, when 2 physical cores are still
//           left for the rest).
//           hle_os_thread.cpp holds each guest thread to its core mask's ones at a host priority from its guest one, so
//           two threads of one guest core never run at once and a low-priority one waits for the higher, as on the
//           console (the #51 / #52 race family). Not placed (logged) without CPU Sets, on Wine, or without the cores.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CPU_HOT = 1, CPU_HELPER = 2, CPU_ANY = 3, CPU_RENDER = 4 };

// Once, early (rt_main.cpp): reads the topology, logs the plan, sets the process default (hot) CPU Sets.
void cpu_place_init(void);
// The calling thread: CPU_HELPER moves it to the helper cores, CPU_ANY lets it use every core (both no-ops where
// nothing is restricted); CPU_HOT puts it back on the hot ones and opts it out of EcoQoS; CPU_RENDER: the render
// core (as CPU_HOT where there is none), opted out of EcoQoS.
void cpu_place_thread(int cls);
// The logical processors a class runs on (all of the process affinity's where it is not restricted): for sizing a pool.
// Every class is inside the process affinity mask (boot.py --affinity's PC tiers).
int cpu_place_count(int cls);
// The helpers have cores of their own, apart from the hot ones (placement on, a helper set and a restricted hot set):
// pools sized from CPU_HELPER only then (the precompile). False on one kind of core and one L3, on a hybrid part with
// fewer than 6 P-cores (hot unrestricted: the game shares the E-cores), or with TOTK_CPU_PLACE=0.
bool cpu_place_active(void);
// The hot CPU Set ids (up to n) into ids; 0 when hot is not restricted (exec.c render_pin keeps its rest inside them).
int cpu_place_hot(uint32_t *ids, int n);
// 1: the fastest class's cores in the process affinity have SMT siblings; 0: none (an i7-9700K's 8c/8t, a 285K's
// P-cores, a 4c/4t); -1: unknown (no topology, not Windows). hle_os_thread.cpp's yield default reads it.
int cpu_place_smt(void);
// TOTK_CORE_MODEL=switch placed (the guest cores reserved at cpu_place_init); false: off or not placed.
bool cpu_place_core_model(void);
// Thread h (a real handle, or GetCurrentThread() for the caller) onto the guest cores of mask bits 0..2; false (nothing
// done): the model is off / not placed, or the mask has none of them.
bool cpu_place_guest_thread(void *h, uint64_t mask);

// The planner on its own (tools/engine/test_cpu_place.py): one entry per logical processor.
typedef struct cpu_lp {
    uint32_t id;       // CPU Set id
    uint16_t group;    // processor group (CoreIndex and LastLevelCacheIndex count within it)
    uint16_t core;     // CoreIndex (SMT siblings share it)
    uint16_t llc;      // LastLevelCacheIndex
    uint8_t eff;       // EfficiencyClass: higher = faster core (all 0 on a CPU with one kind)
    uint8_t parked;    // (unused by the plan; logged)
    uint32_t llc_kb;   // the size of that last-level cache, KB (0: unknown)
    uint8_t lp;        // LogicalProcessorIndex (LARECOMP_RENDER_PIN=<n>)
} cpu_lp;
typedef struct cpu_plan {
    int n_hot, n_helper;   // 0: not restricted (every logical processor)
    int hybrid, llcs;      // distinct efficiency classes > 1; distinct last-level caches
    char why[160];         // one line for the log
} cpu_plan;
// in[n] -> plan; hot[] / helper[] get the CPU Set ids (room for n each), counts in the plan
void cpu_place_plan(const cpu_lp *in, int n, cpu_plan *plan, uint32_t *hot, uint32_t *helper);
// in[n] + the plan's hot[n_hot] (0: unrestricted) -> the render core's logical processors into render[] (room for 4),
// their count (0: none, or fewer than min_smt SMT cores to choose from)
int cpu_place_render_core(const cpu_lp *in, int n, const uint32_t *hot, int n_hot, int min_smt, uint32_t *render);
// TOTK_CORE_MODEL=switch's guest cores: in[n] + hot[n_hot] (0: unrestricted) + render[n_render] -> guest[3] (guest core
// k's id) and the ids out of every other set into reserve[] (room for n), their count in *n_reserve. Returns 2: whole
// physical cores reserved (siblings idle), 1: the 3 logical processors only, 0: not placed (too few cores).
int cpu_place_guest_cores(const cpu_lp *in, int n, const uint32_t *hot, int n_hot, const uint32_t *render, int n_render,
                          uint32_t *guest, uint32_t *reserve, int *n_reserve);

#ifdef __cplusplus
}
#endif
