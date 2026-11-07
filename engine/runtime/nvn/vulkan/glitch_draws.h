// TOTK_GLITCH_DRAWS' comparison, without Vulkan or NVN: the recorded
// draws of a frame N against N-1 and N+1, logged as '[glitch]     frame N: ...' lines. tools/engine/glitch_draws_test.cpp
// runs it on synthetic frames (tools/engine/test_glitch_draws.py).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TDR_LOG
#define TDR_LOG(...) printf(__VA_ARGS__)
#endif

enum { TDR_N = 2048, TDR_T = 8, TDR_B = 12, TDR_LINES = 40 };
enum { TDRB_UBO, TDRB_SSBO, TDRB_VTX, TDRB_IDX };
typedef struct { const void *t; uint64_t addr, img, dh; uint32_t handle, w, h, fmt; uint8_t stage, ti, late, tile; } TdrTex;
typedef struct { uint64_t addr, size, h, hashed; uint8_t kind, stage, slot; } TdrBuf;
typedef struct {
    uint64_t vs, fs, rt, tile, iaddr, idx;
    uint32_t rtw, rth, count, first, inst, base_inst;
    int32_t base_vertex;
    uint8_t indexed, nt, nb;
    TdrTex t[TDR_T];
    TdrBuf b[TDR_B];
} TdrDraw;
typedef struct { int n, dropped; uint64_t hashed; TdrDraw d[TDR_N]; } TdrFrame;   // (hashed: bytes, the frame's cost)
static bool tdr_same(const TdrDraw *a, const TdrDraw *b) { return a->vs == b->vs && a->fs == b->fs && a->rt == b->rt && a->tile == b->tile; }
static void tdr_rank(const TdrFrame *f, uint16_t *rk) {   // (each draw's rank among the frame's draws of its programs, target and tile)
    for (int i = 0; i < f->n; i++) {
        int k = 0;
        for (int j = 0; j < i; j++) k += tdr_same(&f->d[j], &f->d[i]);
        rk[i] = (uint16_t)k;
    }
}
static int tdr_find(const TdrFrame *f, const uint16_t *rk, const TdrDraw *d, int r) {
    for (int i = 0; i < f->n; i++)
        if (rk[i] == r && tdr_same(&f->d[i], d)) return i;
    return -1;
}
static const TdrTex *tdr_tex(const TdrDraw *d, int sg, int ti) {
    for (int k = 0; k < d->nt; k++)
        if (d->t[k].stage == sg && d->t[k].ti == ti) return &d->t[k];
    return NULL;
}
static const TdrBuf *tdr_bufat(const TdrDraw *d, int kind, int sg, int slot) {
    for (int k = 0; k < d->nb; k++)
        if (d->b[k].kind == kind && d->b[k].stage == sg && d->b[k].slot == slot) return &d->b[k];
    return NULL;
}
static bool tdr_tex_eq(const TdrTex *a, const TdrTex *b) {
    if (!a || !b) return a == b;
    return a->handle == b->handle && a->t == b->t && a->addr == b->addr && a->w == b->w && a->h == b->h && a->fmt == b->fmt &&
           a->img == b->img && a->dh == b->dh && a->late == b->late;
}
static const char *tdr_tex_why(const TdrTex *a, const TdrTex *b) {   // (a neighbour's value -> N's)
    if (!a || !b) return "kept in one record only (its list of 8 was full)";
    if (a->handle != b->handle) return "the list bound another handle";
    if (a->t != b->t) return "same handle, the pool entry holds another object";
    if (a->addr != b->addr || a->w != b->w || a->h != b->h || a->fmt != b->fmt) return "same object, other storage / shape";
    if (a->img != b->img) return "same object, another image behind it (alias writer / image made again)";
    if (a->dh != b->dh) return "same image, other bytes (uploaded between)";
    return "the pool entry changed after the list's submit (LATE) in one of them";
}
static int tdr_tex_str(char *o, size_t n, const TdrTex *t) {
    if (!t) return snprintf(o, n, "-");
    return snprintf(o, n, "h %x obj %p %llx %ux%u f%#x img %llx bytes %llx%s", t->handle, (const void *)t->t, (unsigned long long)t->addr,
                    t->w, t->h, t->fmt, (unsigned long long)t->img, (unsigned long long)t->dh, t->late ? " LATE" : "");
}
static bool tdr_buf_eq(const TdrBuf *a, const TdrBuf *b) {
    if (!a || !b) return a == b;
    return a->addr == b->addr && a->size == b->size && a->h == b->h && a->hashed == b->hashed;
}
static const char *tdr_buf_why(const TdrBuf *a, const TdrBuf *b) {
    if (!a || !b) return "kept in one record only (its list of 12 was full)";
    if (a->addr != b->addr) return "the list bound another buffer";
    if (a->size != b->size || a->hashed != b->hashed) return "same address, another bound size";
    return "same buffer, other bytes (written between: the game's CPU, or GPU writes copied back)";
}
static int tdr_buf_str(char *o, size_t n, const TdrBuf *b) {
    if (!b) return snprintf(o, n, "-");
    return snprintf(o, n, "%llx +%llx bytes %llx (%llu hashed)", (unsigned long long)b->addr, (unsigned long long)b->size,
                    (unsigned long long)b->h, (unsigned long long)b->hashed);
}
static bool tdr_par_eq(const TdrDraw *a, const TdrDraw *b) {
    return a->indexed == b->indexed && a->count == b->count && a->first == b->first && a->base_vertex == b->base_vertex &&
           a->inst == b->inst && a->base_inst == b->base_inst;
}
static int tdr_par_str(char *o, size_t n, const TdrDraw *d) {
    return snprintf(o, n, "%s count %u first %u base vertex %d instances %u from %u", d->indexed ? "indexed" : "arrays", d->count,
                    d->first, d->base_vertex, d->inst, d->base_inst);
}
static int tdr_head(char *o, size_t n, const TdrDraw *d, int rank) {
    return snprintf(o, n, "draw #%llu vs %llx fs %016llx rt %llx %ux%u tile %llx (#%d of these)", (unsigned long long)d->idx,
                    (unsigned long long)d->vs, (unsigned long long)d->fs, (unsigned long long)d->rt, d->rtw, d->rth,
                    (unsigned long long)d->tile, rank);
}
static const char *const tdr_kname[] = {"uniform c", "storage s", "vertex stream ", "index"};
// The values where b (frame N) differs from a (N-1) and, with c (N+1), where a and c agree; c NULL: every value b differs
// from a in (a draw that took another's place). One line each while lines last; returns how many differed.
static int tdr_fields(uint64_t frame, const char *head, const TdrDraw *a, const TdrDraw *b, const TdrDraw *c, int *lines, int *every) {
    char va[200], vb[200], vc[200];
    int nd = 0;
#define TDR_DIFF(eq_ab, eq_cb, eq_ac) (c ? !(eq_ab) && !(eq_cb) && (eq_ac) : !(eq_ab))
#define TDR_EVERY(eq_ab, eq_cb, eq_ac) (c && !(eq_ab) && !(eq_cb) && !(eq_ac))
    {
        const bool ab = tdr_par_eq(a, b), cb = c ? tdr_par_eq(c, b) : true, ac = c ? tdr_par_eq(a, c) : true;
        if (TDR_EVERY(ab, cb, ac)) (*every)++;
        if (TDR_DIFF(ab, cb, ac)) {
            nd++;
            if ((*lines)++ < TDR_LINES) {
                tdr_par_str(va, sizeof va, a), tdr_par_str(vb, sizeof vb, b);
                if (c) tdr_par_str(vc, sizeof vc, c);
                TDR_LOG("[glitch]     frame %llu: %s: the draw's own counts (its command in the list): N-1 %s | N %s%s%s\n",
                    (unsigned long long)frame, head, va, vb, c ? " | N+1 " : "", c ? vc : "");
            }
        }
    }
    for (int sg = 0; sg < 2; sg++)
        for (int ti = 0; ti < 64; ti++) {
            const TdrTex *ta = tdr_tex(a, sg, ti), *tb = tdr_tex(b, sg, ti), *tc = c ? tdr_tex(c, sg, ti) : NULL;
            if (!ta && !tb && !tc) continue;
            const bool ab = tdr_tex_eq(ta, tb), cb = c ? tdr_tex_eq(tc, tb) : true, ac = c ? tdr_tex_eq(ta, tc) : true;
            if (TDR_EVERY(ab, cb, ac)) (*every)++;
            if (!TDR_DIFF(ab, cb, ac)) continue;
            nd++;
            if ((*lines)++ >= TDR_LINES) continue;
            tdr_tex_str(va, sizeof va, ta), tdr_tex_str(vb, sizeof vb, tb);
            if (c) tdr_tex_str(vc, sizeof vc, tc);
            TDR_LOG("[glitch]     frame %llu: %s: %s texture %d%s: %s: N-1 %s | N %s%s%s\n", (unsigned long long)frame, head, sg ? "fs" : "vs",
                ti, (ta && ta->tile) || (tb && tb->tile) ? " (tile)" : "", tdr_tex_why(ta, tb), va, vb, c ? " | N+1 " : "", c ? vc : "");
        }
    for (int kind = 0; kind < 4; kind++)
        for (int sg = 0; sg < 2; sg++)
            for (int sl = 0; sl < 18; sl++) {
                const TdrBuf *ba = tdr_bufat(a, kind, sg, sl), *bb = tdr_bufat(b, kind, sg, sl), *bc = c ? tdr_bufat(c, kind, sg, sl) : NULL;
                if (!ba && !bb && !bc) continue;
                const bool ab = tdr_buf_eq(ba, bb), cb = c ? tdr_buf_eq(bc, bb) : true, ac = c ? tdr_buf_eq(ba, bc) : true;
                if (TDR_EVERY(ab, cb, ac)) (*every)++;
                if (!TDR_DIFF(ab, cb, ac)) continue;
                nd++;
                if ((*lines)++ >= TDR_LINES) continue;
                tdr_buf_str(va, sizeof va, ba), tdr_buf_str(vb, sizeof vb, bb);
                if (c) tdr_buf_str(vc, sizeof vc, bc);
                char nm[32];
                if (kind == TDRB_IDX) snprintf(nm, sizeof nm, "%s", tdr_kname[kind]);
                else snprintf(nm, sizeof nm, "%s%d", tdr_kname[kind], sl);
                TDR_LOG("[glitch]     frame %llu: %s: %s %s: %s: N-1 %s | N %s%s%s\n", (unsigned long long)frame, head, sg ? "fs" : "vs", nm,
                    tdr_buf_why(ba, bb), va, vb, c ? " | N+1 " : "", c ? vc : "");
            }
#undef TDR_DIFF
#undef TDR_EVERY
    return nd;
}
typedef struct { int both, onef, oned, every, onlyn, took, missing, lines; } TdrCounts;
// frame N against N-1 (P) and N+1 (X); lines from c->lines on (TDR_LINES or more: counts only, nothing logged)
static void tdr_diff(uint64_t frame, const TdrFrame *P, const TdrFrame *N, const TdrFrame *X, TdrCounts *c) {
    static uint16_t rp[TDR_N], rn[TDR_N], rx[TDR_N];
    static int16_t n2p[TDR_N], n2x[TDR_N];
    static uint8_t p_inn[TDR_N], p_inx[TDR_N], p_used[TDR_N];
    tdr_rank(P, rp), tdr_rank(N, rn), tdr_rank(X, rx);
    memset(p_inn, 0, sizeof p_inn), memset(p_used, 0, sizeof p_used);
    for (int i = 0; i < N->n; i++) {
        n2p[i] = (int16_t)tdr_find(P, rp, &N->d[i], rn[i]), n2x[i] = (int16_t)tdr_find(X, rx, &N->d[i], rn[i]);
        if (n2p[i] >= 0) p_inn[n2p[i]] = 1;
    }
    for (int j = 0; j < P->n; j++) p_inx[j] = tdr_find(X, rx, &P->d[j], rp[j]) >= 0;
    char head[200];
    for (int i = 0; i < N->n; i++) {   // (matched in both neighbours: the values only N has)
        if (n2p[i] < 0 || n2x[i] < 0) continue;
        c->both++;
        tdr_head(head, sizeof head, &N->d[i], rn[i]);
        const int nd = tdr_fields(frame, head, &P->d[n2p[i]], &N->d[i], &X->d[n2x[i]], &c->lines, &c->every);
        c->onef += nd, c->oned += nd > 0;
    }
    for (int i = 0; i < N->n; i++) {   // (only in N: the neighbours' draw of its programs and target it took the place of, if any)
        if (n2p[i] >= 0 || n2x[i] >= 0) continue;
        c->onlyn++;
        const TdrDraw *d = &N->d[i];
        int best = -1;
        for (int j = 0; j < P->n; j++) {
            const TdrDraw *q = &P->d[j];
            if (p_inn[j] || !p_inx[j] || p_used[j] || q->vs != d->vs || q->fs != d->fs || q->rt != d->rt) continue;
            if (best < 0 || llabs((long long)q->idx - (long long)d->idx) < llabs((long long)P->d[best].idx - (long long)d->idx)) best = j;
        }
        tdr_head(head, sizeof head, d, rn[i]);
        if (best < 0) {
            if (c->lines++ < TDR_LINES) {
                char v[200];
                tdr_par_str(v, sizeof v, d);
                TDR_LOG("[glitch]     frame %llu: %s: only in N, no draw of these programs and target missing from N to take the place of (%s)\n",
                        (unsigned long long)frame, head, v);
            }
            continue;
        }
        p_used[best] = 1, c->took++;
        if (c->lines++ < TDR_LINES)
            TDR_LOG("[glitch]     frame %llu: %s: only in N, in place of N-1's draw #%llu (tile %llx in N-1 and N+1); what differs from it:\n",
                    (unsigned long long)frame, head, (unsigned long long)P->d[best].idx, (unsigned long long)P->d[best].tile);
        int none = 0;
        tdr_fields(frame, head, &P->d[best], d, NULL, &c->lines, &none);
    }
    for (int j = 0; j < P->n; j++) {   // (in both neighbours, not in N: not drawn, or drawn as another)
        if (p_inn[j] || !p_inx[j]) continue;
        c->missing++;
        if (p_used[j] || c->lines++ >= TDR_LINES) continue;
        char v[200];
        tdr_head(head, sizeof head, &P->d[j], rp[j]), tdr_par_str(v, sizeof v, &P->d[j]);
        TDR_LOG("[glitch]     frame %llu: N-1's %s: in N-1 and N+1, not in N (%s)\n", (unsigned long long)frame, head, v);
    }
}
// frame N (row[3]) against N-1 (row[2]) and N+1 (row[4]), its lines and a summary; the same counts for N-2 against N-3 /
// N-1 (row[0..2]) as the control: what an ordinary frame's comparison gives (draws that alternate between frames, LOD
// changes), to read N's numbers against. what: "terrain draws" / "draws"
static void tdr_compare(uint64_t frame, const char *what, const TdrFrame *const row[5]) {
    char cnt[96], ctl[200] = "not kept";
    int o = 0, dropped = 0;
    for (int r = 1; r < 5; r++) {
        o += row[r] ? snprintf(cnt + o, sizeof cnt - o, "%s%d", r > 1 ? "/" : "", row[r]->n) : snprintf(cnt + o, sizeof cnt - o, "%s-", r > 1 ? "/" : "");
        if (row[r]) dropped += row[r]->dropped;
    }
    const TdrFrame *P = row[2], *N = row[3], *X = row[4];
    if (!P || !N || !X) {
        TDR_LOG("[glitch]     frame %llu draw ring: %s N-2/N-1/N/N+1 %s: N-1 or N+1 not kept, nothing compared\n", (unsigned long long)frame, what, cnt);
        return;
    }
    TdrCounts c = {0};
    tdr_diff(frame, P, N, X, &c);
    if (row[0] && row[1]) {
        TdrCounts k = {0};
        k.lines = TDR_LINES;   // (counts only)
        tdr_diff(frame - 2, row[0], row[1], P, &k);
        snprintf(ctl, sizeof ctl, "%d value(s) only it has in %d draw(s), %d changing every frame, only in it %d (%d in place), not in it %d",
                 k.onef, k.oned, k.every, k.onlyn, k.took, k.missing);
    }
    TDR_LOG("[glitch]     frame %llu draw ring: %s N-2/N-1/N/N+1 %s%s; in both neighbours %d: %d value(s) only N has in %d draw(s), %d changing every "
            "frame; only in N %d (%d in place of a neighbours' draw); in both neighbours, not in N %d%s | control, N-2 against N-3 / N-1: %s | "
            "N hashed %llu KB\n", (unsigned long long)frame, what, cnt, dropped ? " (a list full: some not kept)" : "", c.both, c.onef, c.oned,
            c.every, c.onlyn, c.took, c.missing, c.lines > TDR_LINES ? "; more lines than shown" : "", ctl, (unsigned long long)(N->hashed >> 10));
}
