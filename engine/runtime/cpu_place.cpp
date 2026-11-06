// cpu_place.cpp: the hot / helper split of cpu_place.h. Windows: GetSystemCpuSetInformation for the cores, their
// efficiency class and last-level cache, GetLogicalProcessorInformationEx for that cache's size; the process default
// CPU Sets for hot, SetThreadSelectedCpuSets for helpers. Both are soft (no affinity mask), and a CPU Set id comes from
// the system's own list, so nothing names a core a 6- or 8-core part lacks. Linux: the plan only (no placement yet).
#include "cpu_place.h"
#include "host_wine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::vector<uint32_t> g_hot, g_helper, g_all, g_render;
uint32_t g_guest[3];   // TOTK_CORE_MODEL=switch: guest core k's CPU Set id (g_guest_on)
int g_total = 0, g_smt = -1;
bool g_on = false, g_guest_on = false;
bool g_split = false;  // something is out of every other set (the render core, the guest cores)

// distinct physical cores among the logical processors of class eff (and cache llc, or any: -1); core / llc are the
// planner's dense ids (below ncore)
int cores_of(const cpu_lp *in, int n, int ncore, int eff, int llc) {
    std::vector<char> seen(ncore);
    int c = 0;
    for (int i = 0; i < n; i++)
        if (in[i].eff == eff && (llc < 0 || in[i].llc == llc) && !seen[in[i].core]) { seen[in[i].core] = 1; c++; }
    return c;
}
// the index of key in keys, appended when new (the planner's dense core / cache ids)
uint16_t dense(std::vector<uint32_t> &keys, uint32_t key) {
    for (size_t i = 0; i < keys.size(); i++) if (keys[i] == key) return (uint16_t)i;
    keys.push_back(key);
    return (uint16_t)(keys.size() - 1);
}

}  // namespace

// The rules (cpu_place.h): hot = the fastest efficiency class, and of those the one last-level cache with the most L3
// (then the most logical processors, then the lowest index) when that cache has at least 6 physical cores; a hybrid
// part with fewer than 6 fast cores leaves hot unrestricted (its few P-cores would be oversubscribed while the E-cores
// idle: i5-1235U 2P, Lunar Lake 4P, Ryzen AI 9 HX 370's 4 Zen 5). helper = the slower classes on a hybrid part (those
// sharing a cache with a fast core first: Meteor Lake's low-power island is the last resort), else the caches hot does
// not use. A "slower" class owning the bigger cache is not an efficiency core (a dual-CCD X3D whose frequency CCD
// reports a higher class): then the classes are ignored and the caches decide. Unrestricted = 0 ids.
extern "C" void cpu_place_plan(const cpu_lp *in_raw, int n, cpu_plan *plan, uint32_t *hot, uint32_t *helper) {
    memset(plan, 0, sizeof *plan);
    if (n <= 0) { snprintf(plan->why, sizeof plan->why, "no topology: nothing placed"); return; }
    int top = 0, lo = 255;
    uint32_t top_kb = 0, low_kb = 0;
    for (int i = 0; i < n; i++) { top = in_raw[i].eff > top ? in_raw[i].eff : top; lo = in_raw[i].eff < lo ? in_raw[i].eff : lo; }
    for (int i = 0; i < n; i++) {
        uint32_t &kb = in_raw[i].eff == top ? top_kb : low_kb;
        kb = in_raw[i].llc_kb > kb ? in_raw[i].llc_kb : kb;
    }
    // CoreIndex and LastLevelCacheIndex are group-relative: dense ids keyed by (group, index), so a process over
    // several groups (> 64 logical processors) never merges two groups' cores or caches
    std::vector<cpu_lp> flat(in_raw, in_raw + n);
    std::vector<uint32_t> core_keys, llc_keys;
    for (cpu_lp &l : flat) {
        l.core = dense(core_keys, (uint32_t)l.group << 16 | l.core);
        l.llc = dense(llc_keys, (uint32_t)l.group << 16 | l.llc);
    }
    const int ncore = (int)core_keys.size(), nllc = (int)llc_keys.size();
    if (top != lo && low_kb > top_kb) {
        for (cpu_lp &l : flat) l.eff = (uint8_t)top;
        lo = top;
    }
    const cpu_lp *in = flat.data();
    plan->hybrid = top != lo;
    plan->llcs = nllc;
    int fast = 0;
    for (int i = 0; i < n; i++) fast += in[i].eff == top;
    // the fast cores' caches: the best one
    int best = -1, best_n = 0;
    uint32_t best_kb = 0;
    for (int c = 0; c < nllc; c++) {
        int k = 0;
        uint32_t kb = 0;
        for (int i = 0; i < n; i++) if (in[i].llc == c && in[i].eff == top) { k++; kb = in[i].llc_kb; }
        if (!k) continue;
        if (best < 0 || kb > best_kb || (kb == best_kb && k > best_n)) { best = c; best_n = k; best_kb = kb; }
    }
    int fast_llcs = 0;
    for (int c = 0; c < nllc; c++)
        for (int i = 0; i < n; i++) if (in[i].llc == c && in[i].eff == top) { fast_llcs++; break; }
    const bool one_cache = fast_llcs > 1 && cores_of(in, n, ncore, top, best) >= 6;   // hot on one of several caches
    const bool fast_only = plan->hybrid && cores_of(in, n, ncore, top, -1) >= 6;
    if (one_cache || fast_only)
        for (int i = 0; i < n; i++)
            if (in[i].eff == top && (!one_cache || in[i].llc == best)) hot[plan->n_hot++] = in[i].id;
    if (plan->n_hot == n) plan->n_hot = 0;
    if (plan->hybrid) {
        // the slow cores next to a fast one's cache first
        for (int pass = 0; pass < 2 && plan->n_helper < 2; pass++) {
            plan->n_helper = 0;
            for (int i = 0; i < n; i++) {
                if (in[i].eff == top) continue;
                bool by_fast = false;
                for (int j = 0; j < n && !by_fast; j++) by_fast = in[j].eff == top && in[j].llc == in[i].llc;
                if (pass == 1 || by_fast) helper[plan->n_helper++] = in[i].id;
            }
        }
        if (plan->n_helper < 2) plan->n_helper = 0;
    } else if (plan->n_hot) {
        for (int i = 0; i < n; i++) {
            bool in_hot = false;
            for (int j = 0; j < plan->n_hot && !in_hot; j++) in_hot = hot[j] == in[i].id;
            if (!in_hot) helper[plan->n_helper++] = in[i].id;
        }
    }
    snprintf(plan->why, sizeof plan->why, "%d logical, %d fast, %d cache(s)%s: hot = %s, helpers = %s", n, fast,
             plan->llcs, plan->hybrid ? ", hybrid" : "",
             !plan->n_hot ? "all" : one_cache ? (best_kb > 0 ? "one cache (the largest L3)" : "one cache") : "the fast cores",
             !plan->n_helper ? "all" : plan->hybrid ? "the efficiency cores" : "the other cache(s)");
}

// The render core (cpu_place.h): among hot (the fastest class's logical processors where hot is unrestricted), the
// physical cores with two logical processors or more; with >= min_smt of them, the last one (core 0 takes most
// interrupts and the system's own threads; the manual pin was the 9800X3D's last core too).
extern "C" int cpu_place_render_core(const cpu_lp *in, int n, const uint32_t *hot, int n_hot, int min_smt, uint32_t *render) {
    int top = 0;
    for (int i = 0; i < n; i++) top = in[i].eff > top ? in[i].eff : top;
    auto cand = [&](const cpu_lp &l) {
        if (!n_hot) return (int)l.eff == top;
        for (int j = 0; j < n_hot; j++)
            if (hot[j] == l.id) return true;
        return false;
    };
    std::vector<uint32_t> keys;
    std::vector<int> lps;
    for (int i = 0; i < n; i++) {
        if (!cand(in[i])) continue;
        const uint16_t k = dense(keys, (uint32_t)in[i].group << 16 | in[i].core);
        if (k >= lps.size()) lps.resize(k + 1);
        lps[k]++;
    }
    int smt = 0, last = -1;
    for (size_t k = 0; k < lps.size(); k++)
        if (lps[k] >= 2) { smt++; last = (int)k; }
    if (smt < (min_smt > 1 ? min_smt : 1)) return 0;
    int m = 0;
    for (int i = 0; i < n && m < 4; i++)
        if (cand(in[i]) && ((uint32_t)in[i].group << 16 | in[i].core) == keys[last]) render[m++] = in[i].id;
    return m;
}

// The guest cores (cpu_place.h "guest"): three physical cores of hot (all where it is unrestricted), the fastest class
// first, then the system's order, not the render core's; guest core k = the first logical processor of the k-th. Whole
// cores (siblings idle) while 2 physical cores are left for everything else, else the 3 logical processors while 2 of
// those are left (a 4c/8t with its render core), else none.
extern "C" int cpu_place_guest_cores(const cpu_lp *in, int n, const uint32_t *hot, int n_hot, const uint32_t *render, int n_render,
                                     uint32_t *guest, uint32_t *reserve, int *n_reserve) {
    *n_reserve = 0;
    auto has = [](const uint32_t *v, int m, uint32_t id) { for (int j = 0; j < m; j++) if (v[j] == id) return true; return false; };
    int top = 0;
    for (int i = 0; i < n; i++) top = in[i].eff > top ? in[i].eff : top;
    std::vector<uint32_t> cores, cand;   // (group << 16 | core) of every physical core; of the chosen ones
    for (int i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)in[i].group << 16 | in[i].core;
        if (std::find(cores.begin(), cores.end(), k) == cores.end()) cores.push_back(k);
    }
    for (int eff = top; eff >= 0 && cand.size() < 3; eff--)
        for (int i = 0; i < n && cand.size() < 3; i++) {
            const uint32_t k = (uint32_t)in[i].group << 16 | in[i].core;
            if (in[i].eff != eff || (n_hot && !has(hot, n_hot, in[i].id)) || std::find(cand.begin(), cand.end(), k) != cand.end())
                continue;
            bool on_render = false;   // (any logical processor of this core is the render core's)
            for (int j = 0; j < n; j++)
                on_render |= ((uint32_t)in[j].group << 16 | in[j].core) == k && has(render, n_render, in[j].id);
            if (on_render) continue;
            guest[cand.size()] = in[i].id;
            cand.push_back(k);
        }
    const int left = (int)cores.size() - (n_render ? 1 : 0) - 3, lp_left = n - n_render - 3;
    if (cand.size() < 3 || (left < 2 && lp_left < 2)) return 0;
    const bool whole = left >= 2;
    for (int i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)in[i].group << 16 | in[i].core;
        if (whole ? std::find(cand.begin(), cand.end(), k) != cand.end() : has(guest, 3, in[i].id)) reserve[(*n_reserve)++] = in[i].id;
    }
    return whole ? 2 : 1;
}

#ifdef _WIN32
extern "C" bool backend_async_on(void);   // nvn/vulkan/exec.c: the render thread runs (LARECOMP_ASYNC_EXEC, a capture)
namespace {

// The last-level cache size of each logical processor (group, index) -> KB
uint32_t llc_kb_of(WORD group, BYTE lp) {
    static std::vector<uint8_t> buf;
    static bool read = false;
    if (!read) {
        read = true;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
        buf.resize(len);
        if (!len || !GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf.data(), &len))
            buf.clear();
        else buf.resize(len);
    }
    uint32_t kb = 0;
    int level = 0;
    for (size_t at = 0; at < buf.size();) {
        auto *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf.data() + at);
        at += e->Size;
        const CACHE_RELATIONSHIP &c = e->Cache;
        if (c.Type != CacheUnified && c.Type != CacheData) continue;
        if (c.GroupMask.Group != group || !(c.GroupMask.Mask >> lp & 1)) continue;   // (the first group's mask)
        if (c.Level > level || (c.Level == level && c.CacheSize / 1024 > kb)) { level = c.Level; kb = c.CacheSize / 1024; }
    }
    return kb;
}

}  // namespace

extern "C" void cpu_place_init(void) {
    static bool done = false;
    if (done) return;
    done = true;
    auto k32 = GetModuleHandleA("kernel32.dll");
    auto gsi = (BOOL(WINAPI *)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG))GetProcAddress(
        k32, "GetSystemCpuSetInformation");
    auto spd = (BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG))GetProcAddress(k32, "SetProcessDefaultCpuSets");
    // Wine / Proton: SetProcessDefaultCpuSets and SetThreadSelectedCpuSets are stubs that return TRUE and place nothing
    // (kernelbase memory.c), so a plan would size pools to a split (hot, helpers, a free render core) that never happens:
    // nothing placed there, as without CPU Sets
    const bool wine = host_is_wine();
    if (wine) spd = nullptr;
    // Only the logical processors the process may run on (boot.py --affinity's PC tiers, Task Manager, start
    // /affinity): every class, count and CPU Set is inside that mask. A process in several groups (Windows 11 on > 64
    // logical processors) keeps those groups whole (no per-group mask to read).
    USHORT groups[16] = {}, ngroups = 16;
    if (!GetProcessGroupAffinity(GetCurrentProcess(), &ngroups, groups)) ngroups = 0;
    DWORD_PTR pmask = 0, smask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pmask, &smask)) pmask = 0;
    auto allowed = [&](WORD group, BYTE lp) {
        if (!ngroups) return true;
        bool in = false;
        for (USHORT i = 0; i < ngroups; i++) in |= groups[i] == group;
        return in && (ngroups > 1 || !pmask || (pmask >> lp & 1));
    };
    std::vector<cpu_lp> lps;
    int outside = 0;
    if (gsi) {
        ULONG len = 0;
        gsi(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<uint8_t> buf(len);
        if (len && gsi((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0))
            for (ULONG at = 0; at + sizeof(DWORD) * 2 <= len;) {
                auto *e = (SYSTEM_CPU_SET_INFORMATION *)(buf.data() + at);
                if (!e->Size) break;
                at += e->Size;
                if (e->Type != CpuSetInformation) continue;
                if (!allowed(e->CpuSet.Group, e->CpuSet.LogicalProcessorIndex)) { outside++; continue; }
                cpu_lp l = {};
                l.id = e->CpuSet.Id;
                l.group = e->CpuSet.Group;
                l.core = e->CpuSet.CoreIndex;
                l.llc = e->CpuSet.LastLevelCacheIndex;
                l.eff = e->CpuSet.EfficiencyClass;
                l.parked = e->CpuSet.Parked;
                l.llc_kb = llc_kb_of(e->CpuSet.Group, e->CpuSet.LogicalProcessorIndex);
                l.lp = e->CpuSet.LogicalProcessorIndex;
                lps.push_back(l);
                g_all.push_back(l.id);
            }
    }
    g_total = (int)lps.size();
    {   // SMT: two logical processors of the fastest class on one physical core
        int top = 0;
        for (const cpu_lp &l : lps) top = l.eff > top ? l.eff : top;
        std::vector<uint32_t> seen;
        g_smt = lps.empty() ? -1 : 0;
        for (const cpu_lp &l : lps) {
            if (l.eff != top) continue;
            const uint32_t k = (uint32_t)l.group << 16 | l.core;
            for (uint32_t s : seen) g_smt |= s == k;
            seen.push_back(k);
        }
    }
    g_hot.resize(lps.size());
    g_helper.resize(lps.size());
    cpu_plan plan;
    cpu_place_plan(lps.data(), (int)lps.size(), &plan, g_hot.data(), g_helper.data());
    g_hot.resize(plan.n_hot);
    g_helper.resize(plan.n_helper);
    const char *e = getenv("TOTK_CPU_PLACE");
    g_on = !(e && *e == '0') && spd;
    uint32_t big = 0;
    for (const cpu_lp &l : lps) big = l.llc_kb > big ? l.llc_kb : big;
    printf("[cpu] %s; largest last-level cache %u KB%s\n", plan.why, big,
           !g_on ? (spd ? " (TOTK_CPU_PLACE=0: nothing placed)" : wine ? " (Wine: its CPU Sets are stubs, nothing placed)"
                                                              : " (no CPU Sets: nothing placed)") : "");
    // the render core (cpu_place.h): out of every other set, the process default included
    const char *rp = getenv("LARECOMP_RENDER_PIN");
    const bool render_thread = backend_async_on();   // (exec.c: whether there is a render thread at all)
    const bool r_off = rp && !strcmp(rp, "0"), r_auto = rp && !strcmp(rp, "auto"), manual = rp && *rp && !r_off && !r_auto;
    // (>= 4 SMT cores since v0.1.8: on 4c/8t the render thread is the cap, ~90 % of the frame with the GPU half idle and
    // the game's threads blocked ~2/3 of their time (db_lc4_prof, t3_fight_t4b stacks), and next to a busy sibling it ran
    // 18 % slower a frame (pin_kak_t4); =auto is the same now, =0 the old 4-5 core behaviour (none))
    const int min_smt = 4;
    uint32_t rids[4];
    int nr = 0;
    if (spd && render_thread && !r_off) {
        if (manual) {   // (the core holding logical processor n)
            const int want = atoi(rp);
            const cpu_lp *at = nullptr;
            for (const cpu_lp &l : lps)
                if (!at && l.lp == want) at = &l;
            for (const cpu_lp &l : lps)
                if (at && nr < 4 && l.group == at->group && l.core == at->core) rids[nr++] = l.id;
        } else {
            nr = cpu_place_render_core(lps.data(), (int)lps.size(), g_on ? g_hot.data() : nullptr, g_on ? (int)g_hot.size() : 0,
                                       min_smt, rids);
        }
    }
    std::vector<uint32_t> out;   // (out of every other set)
    if (nr && nr < (int)lps.size()) {
        g_render.assign(rids, rids + nr);
        out = g_render;
    }
    // TOTK_CORE_MODEL=switch (cpu_place.h "guest"): the guest cores out of every other set, as the render core
    const char *cm = getenv("TOTK_CORE_MODEL");
    const char *cm_why = nullptr;
    int cm_mode = 0;
    if (cm && !strcmp(cm, "switch")) {
        std::vector<uint32_t> res(lps.size());
        int nres = 0;
        cm_mode = cpu_place_guest_cores(lps.data(), (int)lps.size(), g_on ? g_hot.data() : nullptr, g_on ? (int)g_hot.size() : 0,
                                        g_render.data(), (int)g_render.size(), g_guest, res.data(), &nres);
        if (!spd) cm_why = wine ? "Wine: its CPU Sets are stubs" : "no CPU Sets";
        else if (!cm_mode) cm_why = "too few cores: 3 physical ones beside the render core, and 2 left for the rest";
        else {
            out.insert(out.end(), res.begin(), res.begin() + nres);
            g_guest_on = true;
        }
    }
    if (!out.empty()) {
        g_split = true;
        auto drop = [&out](std::vector<uint32_t> &v) {
            std::vector<uint32_t> keep;
            for (uint32_t id : v) {
                bool r = false;
                for (uint32_t x : out) r |= x == id;
                if (!r) keep.push_back(id);
            }
            v.swap(keep);
        };
        if (!g_on || g_hot.empty()) g_hot = g_all;   // (hot restricted now: everything but those)
        drop(g_hot);
        drop(g_all);
        drop(g_helper);
        if (g_hot.empty()) g_hot = g_all;   // (the guest cores took all of hot: the rest of the process instead)
    }
    printf("[cpu] render thread: %s\n",
           !g_render.empty() ? (manual ? "alone on the core of LARECOMP_RENDER_PIN's logical processor"
                                  : "alone on the last SMT core of hot (LARECOMP_RENDER_PIN=0: off)")
           : r_off           ? "not placed (LARECOMP_RENDER_PIN=0)"
           : !render_thread  ? "none (synchronous execute)"
           : !spd            ? (wine ? "not placed (Wine: its CPU Sets are stubs)" : "not placed (no CPU Sets)")
           : manual          ? "not placed (LARECOMP_RENDER_PIN: no such logical processor)"
                             : "not placed (fewer than 4 SMT cores in hot)");
    if (g_guest_on)
        printf("[cpu] TOTK_CORE_MODEL=switch: guest cores 0 / 1 / 2 on CPU Sets %u / %u / %u alone (%s); guest threads held "
               "to their core mask there at host priorities from the guest ones\n",
               g_guest[0], g_guest[1], g_guest[2], cm_mode == 2 ? "their physical cores reserved" : "their SMT siblings run the rest");
    else if (cm_why)
        printf("[cpu] TOTK_CORE_MODEL=switch: not placed (%s): guest threads run as without it\n", cm_why);
    printf("[cpu] counts: hot %d, any %d, helper %d of %d logical processors in the process affinity (%d outside it)%s\n",
           cpu_place_count(CPU_HOT), cpu_place_count(CPU_ANY), cpu_place_count(CPU_HELPER), g_total, outside,
           cpu_place_active() ? "" : "; helpers not separated");
    printf("[cpu] SMT: %s\n", g_smt > 0 ? "yes (the fastest cores have siblings)" : g_smt == 0 ? "no (one logical processor a core)" : "unknown");
    if ((g_on || g_split) && !g_hot.empty()) {
        if (!spd(GetCurrentProcess(), (const ULONG *)g_hot.data(), (ULONG)g_hot.size()))
            printf("[cpu] SetProcessDefaultCpuSets failed (%lu)\n", GetLastError());
        // The process default is documented for threads created after it, so the thread calling this (the main /
        // window thread) gets the hot set itself. (A host test on Windows 10 19045 saw a thread started before the
        // call follow the default too: 0 of 2579 samples outside it.)
        if (auto sts = (BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG))GetProcAddress(k32, "SetThreadSelectedCpuSets"))
            sts(GetCurrentThread(), (const ULONG *)g_hot.data(), (ULONG)g_hot.size());
    }
}

extern "C" bool cpu_place_core_model(void) { return g_guest_on; }

extern "C" bool cpu_place_guest_thread(void *h, uint64_t mask) {
    if (!g_guest_on) return false;
    static auto sts = (BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                                    "SetThreadSelectedCpuSets");
    ULONG ids[3], n = 0;
    for (int k = 0; k < 3; k++)
        if (mask >> k & 1) ids[n++] = g_guest[k];
    return sts && n && sts((HANDLE)h, ids, n);
}

extern "C" int cpu_place_smt(void) { return g_smt; }

extern "C" int cpu_place_hot(uint32_t *ids, int n) {
    if (!g_on || g_hot.empty()) return 0;
    const int k = (int)g_hot.size() < n ? (int)g_hot.size() : n;
    memcpy(ids, g_hot.data(), k * sizeof *ids);
    return k;
}

extern "C" void cpu_place_thread(int cls) {
    const bool render = cls == CPU_RENDER && !g_render.empty();
    if (cls == CPU_RENDER && !render) cls = CPU_HOT;
    if (!g_on && !render) return;
    static auto sts = (BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                                    "SetThreadSelectedCpuSets");
    static auto sti = (BOOL(WINAPI *)(HANDLE, int, void *, DWORD))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                                "SetThreadInformation");
    if (cls == CPU_HELPER || cls == CPU_ANY) {
        if (!g_on) return;   // (TOTK_CPU_PLACE=0 with a render core: the process default keeps them off it)
        const std::vector<uint32_t> &set = cls == CPU_HELPER ? g_helper : g_all;
        if (sts && !set.empty() && (cls == CPU_HELPER || !g_hot.empty()))
            sts(GetCurrentThread(), (const ULONG *)set.data(), (ULONG)set.size());
        return;
    }
    if (sts && render) sts(GetCurrentThread(), (const ULONG *)g_render.data(), (ULONG)g_render.size());
    else if (sts) sts(GetCurrentThread(), nullptr, 0);   // (back to the process default: hot)
    if (sti) {
        struct { ULONG ver, control, state; } st = {1, 0x1, 0};   // THREAD_POWER_THROTTLING_EXECUTION_SPEED off
        sti(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &st, sizeof st);
    }
}

// Helpers and hot on different cores: a helper set and a restricted hot set. A hybrid part with < 6 P-cores has the
// first without the second (hot unrestricted runs on those same E-cores too).
extern "C" bool cpu_place_active(void) { return g_on && !g_helper.empty() && !g_hot.empty(); }

extern "C" int cpu_place_count(int cls) {
    const bool split = g_on || g_split;   // (any: all but the render / guest cores)
    const size_t n = cls == CPU_HELPER ? (g_on ? g_helper.size() : 0)
                     : cls == CPU_HOT  ? (split ? g_hot.size() : 0)
                     : cls == CPU_ANY && g_split ? g_all.size() : 0;
    if (n) return (int)n;
    if (g_split) return (int)g_all.size();   // (an unrestricted class: every logical processor but the render / guest cores')
    if (g_total) return g_total;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#else
extern "C" void cpu_place_init(void) {}
extern "C" void cpu_place_thread(int) {}
extern "C" bool cpu_place_active(void) { return false; }
extern "C" int cpu_place_hot(uint32_t *, int) { return 0; }
extern "C" int cpu_place_smt(void) { return -1; }
extern "C" bool cpu_place_core_model(void) { return false; }
extern "C" bool cpu_place_guest_thread(void *, uint64_t) { return false; }
extern "C" int cpu_place_count(int) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
