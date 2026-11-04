// Discord status (Rich Presence), opt-in: Game > PC > "Discord status", off by default (TOTK_DISCORD). While on and
// Discord's desktop app runs, the player's profile shows "Playing <the port's Discord application>" and the time played
// this session, with a "Join the community" link button (the port's subreddit); nothing else is sent (no location, no
// save, no account data). Off clears the status at once; the status also goes when the process ends (Discord drops a
// closed pipe's activity).
//
// Discord's local IPC, no SDK: the named pipe \\.\pipe\discord-ipc-0..9, frames of { uint32 opcode, uint32 length }
// + JSON (0 handshake, 1 frame, 2 close, 3 ping, 4 pong), the handshake answered by a READY dispatch, then a
// SET_ACTIVITY command. One thread, started at the first switch-on, sleeping while off; a closed or missing Discord is
// retried every 15 s. The row is registered only when an application id is known (kAppId, or TOTK_DISCORD_APP).
// The most-reacted open request on valentderah/totk-pc-recomp (#377); this implementation is our own.
#include "pc_settings.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

namespace {

// The port's application on discord.com/developers: Discord names the game after it. Public (every client sends it to
// Discord in the clear), not a secret. TOTK_DISCORD_APP overrides it. Its art asset "logo" (the port's own Ouroboros
// logo, uploaded there; never Nintendo art) is the profile's picture; until it exists Discord shows the status without
// one.
const char kAppId[] = "1566914147613147266";

std::string app_id() {
    const char *e = getenv("TOTK_DISCORD_APP");
    std::string id = e && *e ? e : kAppId;
    if (id.size() < 17 || id.size() > 20) return {};
    for (char c : id)
        if (c < '0' || c > '9') return {};
    return id;
}

std::mutex g_mu;
std::condition_variable g_cv;
bool g_on = false, g_started = false;

bool wanted() { std::lock_guard<std::mutex> lk(g_mu); return g_on; }
// Sleeps up to ms; true when the switch changed (or is off) so the caller acts at once.
bool nap(int ms, bool was_on) {
    std::unique_lock<std::mutex> lk(g_mu);
    return g_cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return g_on != was_on; });
}

bool write_all(HANDLE h, const void *p, DWORD n) {
    const char *c = (const char *)p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(h, c, n, &w, nullptr) || !w) return false;
        c += w, n -= w;
    }
    return true;
}
bool read_all(HANDLE h, void *p, DWORD n) {
    char *c = (char *)p;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, c, n, &r, nullptr) || !r) return false;
        c += r, n -= r;
    }
    return true;
}
bool send(HANDLE h, uint32_t op, const std::string &json) {
    uint32_t head[2] = {op, (uint32_t)json.size()};
    return write_all(h, head, sizeof head) && write_all(h, json.data(), (DWORD)json.size());
}
// 1: a frame read into op / body; 0: none waiting; -1: the pipe is gone or the frame is not Discord's.
int poll_frame(HANDLE h, uint32_t &op, std::string &body) {
    DWORD avail = 0;
    uint32_t head[2];
    DWORD got = 0;
    if (!PeekNamedPipe(h, head, sizeof head, &got, &avail, nullptr)) return -1;
    if (got < 8) return 0;
    if (head[1] > (1u << 16)) return -1;
    if (avail < 8 + head[1]) return 0;   // the whole frame first: a Discord stalled mid-frame never holds the thread
    if (!read_all(h, head, sizeof head)) return -1;
    body.resize(head[1]);
    if (head[1] && !read_all(h, &body[0], head[1])) return -1;
    op = head[0];
    return 1;
}

unsigned g_nonce = 0;
// The status's link button (Discord allows two, a label of up to 32 characters): the players' community, as the
// launcher's and Setup's Community buttons. In English: the people who see it are the player's Discord friends, whose
// language the game does not know.
const char kCommunityUrl[] = "https://www.reddit.com/r/WeepingOfTheEmpire/";
std::string activity(bool on, long long start) {
    char b[640];
    if (on)
        snprintf(b, sizeof b, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"timestamps\":{\"start\":%lld},"
                 "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Weeping of the Empire\"},"
                 "\"buttons\":[{\"label\":\"Join the community\",\"url\":\"%s\"}]}},\"nonce\":\"%u\"}",
                 GetCurrentProcessId(), start, kCommunityUrl, ++g_nonce);
    else   // no activity: Discord clears the status
        snprintf(b, sizeof b, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":\"%u\"}", GetCurrentProcessId(),
                 ++g_nonce);
    return b;
}

// The first discord-ipc-N that answers the handshake with READY; INVALID_HANDLE_VALUE when none does. bad_id: Discord
// closed the handshake (an application id it does not know): not retried this session.
HANDLE connect(const std::string &id, bool &bad_id) {
    const char *base = getenv("TOTK_DISCORD_PIPE");   // tools/engine/test_discord.py's stand-in server
    if (!base || !*base) base = "discord-ipc";
    for (int i = 0; i < 10; i++) {
        wchar_t name[96];
        swprintf(name, 96, L"\\\\.\\pipe\\%.60hs-%d", base, i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (send(h, 0, "{\"v\":1,\"client_id\":\"" + id + "\"}")) {
            for (int t = 0; t < 100; t++) {   // up to 5 s for the answer
                uint32_t op = 0;
                std::string body;
                int r = poll_frame(h, op, body);
                if (r < 0) break;
                if (r == 0) { Sleep(50); continue; }
                if (op == 1 && body.find("\"READY\"") != std::string::npos) return h;
                if (op == 2) {
                    printf("[discord] Discord refused the handshake: %.200s\n", body.c_str());
                    bad_id = true;
                }
                break;
            }
        }
        CloseHandle(h);
        if (bad_id) break;
    }
    return INVALID_HANDLE_VALUE;
}

void run() {
    const std::string id = app_id();
    // The time played counts from the game's start, also when the row is turned on later in the session
    long long start = (long long)time(nullptr);
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))   // 100 ns since 1601 -> Unix seconds
        start = (long long)((((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime) / 10000000ull) - 11644473600ll;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool bad_id = false, told = false;
    for (;;) {
        bool on = wanted();
        if (!on) {
            if (pipe != INVALID_HANDLE_VALUE) {
                send(pipe, 1, activity(false, 0));
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                printf("[discord] status cleared\n");
            }
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_on; });
            continue;
        }
        if (bad_id) { nap(3600 * 1000, true); continue; }
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = connect(id, bad_id);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!told) {
                    printf("[discord] Discord is not running: retried every 15 s\n");
                    told = true;
                }
                nap(15000, true);
                continue;
            }
            if (!send(pipe, 1, activity(true, start))) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; continue; }
            printf("[discord] status set\n");
            told = false;
        }
        // Answer pings, notice a closed pipe (Discord quit: reconnect when it is back).
        for (;;) {
            uint32_t op = 0;
            std::string body;
            int r = poll_frame(pipe, op, body);
            if (r == 0) break;
            if (r < 0 || op == 2 || (op == 3 && !send(pipe, 4, body))) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                printf("[discord] Discord closed the connection\n");
                break;
            }
        }
        if (pipe != INVALID_HANDLE_VALUE) nap(1000, true);
    }
}

void set(bool on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_on = on;
    if (on && !g_started) {
        g_started = true;
        std::thread(run).detach();
    }
    g_cv.notify_all();
}

void discord_apply(const char *v) { set(!strcmp(v, "1")); }   // present thread: never blocks

const char *const V_DISCORD[] = {"0", "1", nullptr};
const char *const N_DISCORD[] = {"Off", "On", nullptr};
const pc_setting k_rows[] = {
    {"game.discord", "Game", "PC", "Discord status", "Shows \"Playing\", the time played and a \"Join the community\" "
     "button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.", "0", V_DISCORD, N_DISCORD, 0, 0, 0, 0, "TOTK_DISCORD",
     nullptr, discord_apply, nullptr},
};
struct Rows {
    Rows() { if (!app_id().empty()) pc_settings_add(k_rows, 1); }
} g_rows;

}  // namespace

// rt_main.cpp, once start-up is over: the saved choice (TOTK_DISCORD) takes effect; later changes come through the row.
void pc_discord_start() {
    const char *v = getenv("TOTK_DISCORD");
    if (v && !strcmp(v, "1") && !app_id().empty()) set(true);
}

#else   // Linux: Discord's socket lives in $XDG_RUNTIME_DIR; not wired yet, no row

void pc_discord_start() {}

#endif
