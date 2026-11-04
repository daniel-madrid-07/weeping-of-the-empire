// Shader quarantine (public #34, Intel Arc B580: Intel's shader compiler, igc-default64.dll, crashed the game in a
// play-time pipeline build): the lines of cache/precompile_stuck.txt after its first (the GPU and driver line, exec.c
// pcq_load). A precompile record that got stuck or crashed is its key alone ("<key>": skipped at later starts). A program
// whose driver build crashed or hung is "<key> <level> <shader>: <stage>", and every later build of it on this GPU and
// driver takes that level's form: 1 the driver's optimizer off (VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT: the same
// code, a simpler compile); 2 that, as whole pipelines (no pipeline libraries: another compile path in the driver); 3
// that, built alone: no other driver build in flight while it compiles (Intel's compiler crashed under concurrent builds:
// a thread-safety fault, not the shader, is the likelier cause; public #34 / #39); 4 not built: its draws / dispatches
// are skipped (the least visible loss left; the game keeps running). Each crash moves the program one form on (sq_next),
// each hang too but never to 4 (sq_next_hang), and a crash in a background build (the precompile / archive warm-up /
// prepare, not a build a draw is waiting for) never to 4 either: the game's own build of the same program (the key is
// shared) has not crashed, and a skipped draw is a missing effect (#39's black patches). Files of v0.1.9 and before used
// 3 for 'not built': read now as 'alone', one more try. Pure functions: tools/engine/test_shader_quar.py tests them.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SQ_NOOPT = 1, SQ_WHOLE = 2, SQ_SOLO = 3, SQ_SKIP = 4 };

// a line's key (0: none) and level (0: a precompile record's line)
static inline uint64_t sq_parse(const char *line, int *lvl) {
    char *e;
    const uint64_t k = strtoull(line, &e, 16);
    *lvl = 0;
    if (k && e == line + 16 && *e == ' ') {
        const int l = atoi(e + 1);
        *lvl = l < 0 ? 0 : l > SQ_SKIP ? SQ_SKIP : l;
    }
    return k;
}
static inline int sq_format(char *buf, size_t n, uint64_t key, int lvl, const char *name, const char *what) {
    return snprintf(buf, n, "%016llx %d %s: %s\n", (unsigned long long)key, lvl, name && *name ? name : "?", what ? what : "?");
}
// The form after a crash / hang at form `cur` in a build of stage `what` (build_begin's names). Our GLSL -> SPIR-V
// compiler is not the driver: no driver form changes it. A library or link that failed with the optimizer off tries
// whole pipelines (gpl: the device links libraries at all); anything else that failed with it off is built alone, and
// what failed alone is skipped, unless the crash was in a background build (bg: then alone stays the last form).
// An "engine pipeline" (exec.c vk_engine_*_pipeline: present, post, overlay, decode...) stays at the optimizer-off form:
// the game cannot run without it, so it is never skipped (public #55 follow-up).
static inline int sq_next(int cur, const char *what, int gpl, int bg) {
    if (what && !strncmp(what, "engine", 6)) return SQ_NOOPT;
    if (!what || !strncmp(what, "GLSL", 4)) return SQ_SKIP;
    if (cur <= 0) return SQ_NOOPT;
    if (cur == SQ_NOOPT && gpl && (!strncmp(what, "library", 7) || !strncmp(what, "pipeline link", 13))) return SQ_WHOLE;
    if (cur < SQ_SOLO) return SQ_SOLO;
    return bg ? SQ_SOLO : SQ_SKIP;
}
// The form after a hang (a build still in the driver past the limit): as a crash, but never skipped (a slow build on a
// paging PC whose player closed the "frozen" game is no proof). 0: no safer form left to record.
static inline int sq_next_hang(int cur, const char *what, int gpl) {
    const int n = sq_next(cur, what, gpl, 0);
    return n >= SQ_SKIP ? 0 : n;
}
static inline const char *sq_how(int lvl) {
    return lvl >= SQ_SKIP   ? "not at all (its draws are skipped)"
           : lvl == SQ_SOLO  ? "alone (no other shader build at the same time), whole, with the driver's optimizer off"
           : lvl == SQ_WHOLE ? "as whole pipelines with the driver's optimizer off"
           : lvl == SQ_NOOPT ? "with the driver's optimizer off"
                             : "as usual";
}
// The highest level of key among n lines' keys / levels (0: none)
static inline int sq_level(const uint64_t *keys, const int *lvls, int n, uint64_t key) {
    int l = 0;
    for (int i = 0; key && i < n; i++)
        if (keys[i] == key && lvls[i] > l) l = lvls[i];
    return l;
}
// Rewrites path without the lines after the first that start with prefix; the number removed (-1: not readable)
static inline int sq_drop(const char *path, const char *prefix) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[256], *keep = NULL;
    size_t nk = 0, cap = 0;
    const size_t np = strlen(prefix);
    int removed = 0;
    for (int ln = 0; fgets(line, sizeof line, f); ln++) {
        if (ln && !strncmp(line, prefix, np)) { removed++; continue; }
        const size_t l = strlen(line);
        if (nk + l + 1 > cap) { cap = (nk + l + 1) * 2; keep = (char *)realloc(keep, cap); }
        memcpy(keep + nk, line, l);
        nk += l;
    }
    fclose(f);
    if (removed) {
        char tmp[300];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if ((f = fopen(tmp, "wb"))) {
            if (nk) fwrite(keep, 1, nk, f);
            fclose(f);
            remove(path);
            rename(tmp, path);
        }
    }
    free(keep);
    return removed;
}
