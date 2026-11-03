// pc_amiibo_hold.h: the reader's search as the player sees it (pc_amiibo.cpp; RIVAL-034, rival parity: totk-pc-recomp
// holds 'searching' for 1 s after a stop and logs once). The game may stop and start detection again and again while its
// amiibo screen waits; each StartDetection used to toast and log, and the tab's Scan greyed out between a stop and the
// next start, so a click there was dropped. Now:
//   - a stop with no tag on keeps the tab searching for kHoldMs; a Scan in it waits and goes on at the next start
//     (within kPendingMs of the click);
//   - a start within kQuietMs of such a stop is the same search: no toast, no log line.
// Times are milliseconds on any steady clock. Header only (tools/engine/amiibo_test.cpp).
#pragma once

struct AmiiboHold {
    static constexpr long long kHoldMs = 1000, kPendingMs = 5000, kQuietMs = 10000;
    long long stopped_at = -1;   // the last stop with no tag on (-1: none since the last start)
    long long pending_at = -1;   // a Scan in the hold (-1: none)
    int restarts = 0;            // starts in a row that continued a search

    // StartDetection. Returns true for a new search (toast and log); *deliver: put the waiting dump on now
    bool start(long long now, bool *deliver) {
        const bool again = stopped_at >= 0 && now - stopped_at < kQuietMs;
        *deliver = pending_at >= 0 && now - pending_at < kPendingMs;
        stopped_at = pending_at = -1;
        restarts = again ? restarts + 1 : 0;
        return !again;
    }
    // StopDetection (tag_was_on: a figure was on the reader: that search ended with it, no hold). A stop with no search
    // running (a setup stop, a second stop) changes nothing: the hold and a waiting Scan stay
    void stop(long long now, bool searching, bool tag_was_on) {
        if (!searching && !tag_was_on) return;
        stopped_at = tag_was_on ? -1 : now;
        if (tag_was_on) pending_at = -1;
    }
    // The tab: searching or just stopped (Scan stays enabled)
    bool held(long long now) const { return stopped_at >= 0 && now - stopped_at < kHoldMs; }
    // A Scan between a stop and the next start: true when it waits for that start
    bool queue(long long now) {
        if (!held(now)) return false;
        pending_at = now;
        return true;
    }
    bool waiting(long long now) const { return pending_at >= 0 && now - pending_at < kPendingMs; }
    void reset() { *this = AmiiboHold{}; }
};
