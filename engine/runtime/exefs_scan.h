// exefs_scan.h: the mods' ExeFS patches (.ips, IPS32, IPSwitch .pchtxt) and Atmosphere cheat files for main of TotK (the
// version this build is: MAIN_BIDS), parsed and sorted into what the port can apply. Pure (no runtime, no file system) so tools/engine/test_exefs.py runs it
// without the game; mods_exefs.cpp feeds it the files and the image.
//
// The formats, the build-id match and the offset rule (IPS offsets count the 0x100-byte NSO header; a pchtxt offset adds
// its `@flag offset_shift` first) follow totk-pc-recomp's tools/exefs_patches.py (MIT License,
// Copyright (c) 2026 Valiantsin Dzerakh; see THIRD_PARTY_NOTICES.md).
//
// We are a static recompiler without an interpreter: a write into main's code only takes effect once the patched
// function is recompiled (a patched key, tools/engine/mods_exefs.py), and so does one into .rodata (Image::data_start);
// gen.py never reads .data, so a write there is applied in memory at start. A file is all or nothing: its data writes
// wait until its code writes are built in (half of a patch is worse than none).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace exefs {

constexpr uint32_t NSO_HEADER = 0x100;
#define EXEFS_TITLE "0100F2C0115B6000"
// main's build id per game version the port builds (games/totk/patches/__init__.py, sites_v143.py MAIN_BUILD_ID;
// tools/engine/test_exefs.py checks they agree). Each version is its own recompile with main at the same base, so a mod's
// files for that version (named by its id) apply as they are: no address translation.
constexpr const char* MAIN_BIDS[] = {"082ce09b06e33a123cb1e2770f5f9147709033db",    // 1.0.0
                                     "277178b7dba1b6d4949a84778d4abc58b31b34f5"};   // 1.4.3
constexpr const char* MAIN_BID = MAIN_BIDS[0];
inline const char* g_bid = MAIN_BID;   // this run's (mods_exefs_start: img/main.json's id, one of MAIN_BIDS)
// where a mod keeps its exefs folder, first that exists (rival runtime/mods.c find_root); plus atmosphere/exefs_patches/*/
constexpr const char* PLACES[] = {"exefs", "atmosphere/contents/" EXEFS_TITLE "/exefs", "contents/" EXEFS_TITLE "/exefs",
                                  EXEFS_TITLE "/exefs"};

using Writes = std::map<int64_t, uint8_t>;   // offset in main's image -> byte

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool is_hex(char c) { return (c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'f'); }
inline int hexv(char c) { return c <= '9' ? c - '0' : lower(c) - 'a' + 10; }

// name (hex, any case) is main's build id: the 32-byte form with its zero padding, the 20 bytes, or a cut-down id of at
// least 8 bytes (cheat files use 8, some pchtxt files 16)
inline bool same_build(const std::string& name, const char* bid = g_bid) {
    if (name.size() < 16 || name.size() > 64) return false;
    std::string a, b(bid);
    for (char c : name) { if (!is_hex(c)) return false; a += lower(c); }
    b.resize(64, '0');
    return b.compare(0, a.size(), a) == 0;
}

// the entry of MAIN_BIDS that id (img/main.json's: the 40 digits, or its 64-digit padded form) is, nullptr for another
inline const char* known_build(const std::string& id) {
    if (id.size() < 40) return nullptr;
    for (const char* b : MAIN_BIDS)
        if (same_build(id.substr(0, 40), b)) return b;
    return nullptr;
}

// the cheat file name of this run's build: the first 8 bytes of its id, as Atmosphere writes them
inline std::string cheat_file_of_build() {
    std::string s(g_bid, 16);
    for (char& c : s) if (c >= 'a' && c <= 'f') c = char(c - 32);
    return s + ".txt";
}

// ---- formats

// IPS (PATCH ... EOF, 3-byte offsets) or IPS32 (IPS32 ... EEOF, 4-byte offsets), RLE records too
inline bool parse_ips(const uint8_t* d, size_t n, Writes& w, std::string& err) {
    size_t width, at = 5; const char* end;
    if (n >= 5 && !memcmp(d, "PATCH", 5)) { width = 3; end = "EOF"; }
    else if (n >= 5 && !memcmp(d, "IPS32", 5)) { width = 4; end = "EEOF"; }
    else { err = "not an IPS or IPS32 file"; return false; }
    size_t el = strlen(end);
    for (;;) {
        // the end marker (some tools add a 3-byte truncation size after it)
        if (at + el <= n && !memcmp(d + at, end, el) && at + el + 3 >= n) return true;
        if (at + width + 2 > n) { err = "the IPS file is cut short"; return false; }
        uint32_t off = 0;
        for (size_t i = 0; i < width; i++) off = off << 8 | d[at + i];
        uint32_t size = d[at + width] << 8 | d[at + width + 1];
        at += width + 2;
        if (size) {
            if (at + size > n) { err = "the IPS file is cut short"; return false; }
            for (uint32_t i = 0; i < size; i++) w[int64_t(off) - NSO_HEADER + i] = d[at + i];
            at += size;
        } else {   // a run: 2-byte length, 1 byte
            if (at + 3 > n) { err = "the IPS file is cut short"; return false; }
            uint32_t run = d[at] << 8 | d[at + 1];
            for (uint32_t i = 0; i < run; i++) w[int64_t(off) - NSO_HEADER + i] = d[at + 2];
            at += 3;
        }
    }
}

// the bytes of a pchtxt value: hex digits in byte order, or a quoted string with C escapes
inline bool pchtxt_value(const std::string& t, std::string& out, std::string& err) {
    out.clear();
    if (!t.empty() && t[0] == '"') {
        if (t.size() < 2 || t.back() != '"') { err = "a string without its closing quote: " + t; return false; }
        for (size_t i = 1; i + 1 < t.size(); i++) {
            char c = t[i];
            if (c != '\\' || i + 2 >= t.size()) { out += c; continue; }
            char e = t[++i];
            if (e == 'n') out += '\n'; else if (e == 't') out += '\t'; else if (e == 'r') out += '\r';
            else if (e == '0') out += '\0';
            else if (e == 'x' && i + 2 < t.size() - 1 && is_hex(t[i + 1]) && is_hex(t[i + 2])) {
                out += char(hexv(t[i + 1]) << 4 | hexv(t[i + 2])); i += 2;
            } else out += e;
        }
        return true;
    }
    if (t.empty() || t.size() % 2) { err = "not a hex value: " + t; return false; }
    for (size_t i = 0; i < t.size(); i += 2) {
        if (!is_hex(t[i]) || !is_hex(t[i + 1])) { err = "not a hex value: " + t; return false; }
        out += char(hexv(t[i]) << 4 | hexv(t[i + 1]));
    }
    return true;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

// an IPSwitch file: bid gets its @nsobid- line ("" without one); lines between @enabled and @disabled are written
inline bool parse_pchtxt(const std::string& text, std::string& bid, Writes& w, std::string& err) {
    int64_t shift = 0; bool enabled = false;
    bid.clear();
    size_t p = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;
    while (p <= text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        size_t c = line.find("//"); if (c != std::string::npos) line.resize(c);
        line = trim(line);
        if (line.empty()) continue;
        if (line.compare(0, 5, "@stop") == 0) break;
        if (line.compare(0, 8, "@nsobid-") == 0) bid = trim(line.substr(8));
        else if (line.compare(0, 8, "@enabled") == 0) enabled = true;
        else if (line.compare(0, 9, "@disabled") == 0) enabled = false;
        else if (line.compare(0, 5, "@flag") == 0) {
            std::string rest = trim(line.substr(5));
            if (rest.compare(0, 12, "offset_shift") == 0) shift = (int64_t)strtoll(trim(rest.substr(12)).c_str(), nullptr, 0);
        } else if (line[0] == '@' || !enabled) continue;
        else {
            size_t i = 0;
            while (i < line.size() && is_hex(line[i])) i++;
            if (!i || i >= line.size() || (unsigned char)line[i] > ' ') continue;   // a title in brackets or a line of text
            int64_t off = (int64_t)strtoull(line.substr(0, i).c_str(), nullptr, 16) + shift - NSO_HEADER;
            std::string val = trim(line.substr(i)), bytes;
            if (val[0] != '"') { size_t s = 0; while (s < val.size() && (unsigned char)val[s] > ' ') s++; val.resize(s); }
            if (!pchtxt_value(val, bytes, err)) return false;
            for (size_t k = 0; k < bytes.size(); k++) w[off + (int64_t)k] = (uint8_t)bytes[k];
        }
    }
    return true;
}

// ---- sorting the writes

struct Image {
    const uint8_t* bytes = nullptr; uint32_t size = 0;   // main's image as loaded (img/main.bin): up to the end of .data
    uint32_t text_end = 0;                               // end of .text and its padding (start of .rodata)
    // start of.data: below it a write needs the rebuild. gen.py reads.rodata at run time too, except the br jump
    // tables it lifts into switches when it builds (a table entry changed in memory only would miss its case), so a
    // file's.rodata writes go with its code writes;.data is never read when lifting
    uint32_t data_start = 0;
};
// the segments of img/main.json ("segments": [{"name": "text"|"ro"|"data", "mem": N, "size": N}, ...]): false when
// they are not there
inline bool image_layout(const std::string& json, Image& im) {
    uint64_t mem[3] = {}, size[3] = {};
    bool seen[3] = {};
    const char* names[3] = {"\"text\"", "\"ro\"", "\"data\""};
    for (int i = 0; i < 3; i++) {
        size_t n = json.find(names[i]);
        if (n == std::string::npos) return false;
        size_t end = json.find('}', n), m = json.find("\"mem\"", n), s = json.find("\"size\"", n);
        if (end == std::string::npos || m == std::string::npos || m > end || s == std::string::npos || s > end) return false;
        mem[i] = strtoull(json.c_str() + json.find(':', m) + 1, nullptr, 10);
        size[i] = strtoull(json.c_str() + json.find(':', s) + 1, nullptr, 10);
        seen[i] = true;
    }
    if (!seen[0] || !seen[1] || !seen[2] || !(mem[0] < mem[1] && mem[1] < mem[2])) return false;
    im.text_end = (uint32_t)mem[1];
    im.data_start = (uint32_t)mem[2];
    im.size = (uint32_t)(mem[2] + size[2]);
    return true;
}
struct Site { uint32_t off, len; std::string label; };    // a port patch (the package's patch set)

struct File {
    std::string mod, rel;                 // mod folder name, path in the mod (forward slashes)
    enum Kind { CODE, DATA, OTHER, EMPTY, BAD, OVERLAP } kind = EMPTY;
    Writes code, data;                    // bytes that differ from the image: code needs the rebuild (.text, .rodata)
    std::string note, arg;                // why BAD / OVERLAP: an English text (a %s takes arg), for pc_tr
    int slot = -1;                        // a code cheat's: its switch flag (built in on or off, switched live)
};

// one file's writes against the image: drops the game's own bytes, splits code / data, refuses what we cannot apply
inline void classify(File& f, const Writes& w, const Image& im, const std::vector<Site>& port) {
    f.code.clear(); f.data.clear();
    for (auto& [off, b] : w) {
        if (off < 0) { f.kind = File::BAD; f.note = "It writes the NSO header."; return; }
        if (off >= im.size) { f.kind = File::BAD; f.note = "It writes past main's image (bss)."; return; }
        if (im.bytes[off] == b) continue;
        for (auto& s : port)
            if (off >= s.off && off < s.off + s.len) {
                f.kind = File::BAD; f.note = "It changes code the port already patches (%s)."; f.arg = s.label; return;
            }
        (off < im.data_start ? f.code : f.data)[off] = b;
    }
    f.kind = !f.code.empty() ? File::CODE : !f.data.empty() ? File::DATA : File::EMPTY;
}

// files in mod order (top wins): a file writing a byte an earlier file wrote is left out as a whole. Two code cheats may
// write the same bytes (each is its own switch; the first one on wins), never a cheat and a file
inline void resolve(std::vector<File>& files) {
    std::map<int64_t, size_t> owner, cheat_owner;
    for (size_t i = 0; i < files.size(); i++) {
        File& f = files[i];
        if (f.kind != File::CODE && f.kind != File::DATA) continue;
        const File* hit = nullptr;
        for (auto* w : {&f.code, &f.data})
            for (auto& [off, b] : *w) {
                auto o = owner.find(off);
                if (o != owner.end() || (f.slot < 0 && (o = cheat_owner.find(off)) != cheat_owner.end())) {
                    hit = &files[o->second]; break;
                }
            }
        if (hit) { f.kind = File::OVERLAP; f.note = "It overlaps %s, higher in the list."; f.arg = hit->mod + "/" + hit->rel; continue; }
        for (auto* w : {&f.code, &f.data}) for (auto& [off, b] : *w) (f.slot < 0 ? owner : cheat_owner).emplace(off, i);
    }
}

// the code writes of the usable files, merged; a code cheat's apart (key + (slot + 1) << 40: built in as its switch)
inline Writes code_set(const std::vector<File>& files) {
    Writes out;
    for (auto& f : files)
        if (f.kind == File::CODE)
            for (auto& [off, b] : f.code) out[f.slot < 0 ? off : off + (int64_t(f.slot + 1) << 40)] = b;
    return out;
}

// FNV-1a 64 of the sorted code writes: the identity of a patched build ("" = none)
inline std::string set_hash(const Writes& code) {
    if (code.empty()) return "";
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint8_t b) { h ^= b; h *= 0x100000001b3ull; };
    for (auto& [off, b] : code) { for (int i = 0; i < 8; i++) mix(uint8_t(uint64_t(off) >> (i * 8))); mix(b); }
    char s[17]; snprintf(s, sizeof s, "%016llx", (unsigned long long)h);
    return s;
}

// an exefs-set.txt id ("file <name id> <content>" / "cheat <name id> <content>") without its name part: what the exe
// holds whatever the mod's folder is called now (the name id hashes the mod's name; RIVAL-034)
inline std::string content_id(const std::string& id) {
    const size_t a = id.find(' '), b = a == std::string::npos ? a : id.find(' ', a + 1);
    return b == std::string::npos ? id : id.substr(0, a) + id.substr(b);
}

// runs of consecutive bytes: [(offset, new bytes)]
inline std::vector<std::pair<uint32_t, std::string>> runs(const Writes& w) {
    std::vector<std::pair<uint32_t, std::string>> out;
    for (auto& [off, b] : w) {
        if (out.empty() || out.back().first + out.back().second.size() != (uint64_t)off) out.push_back({(uint32_t)off, ""});
        out.back().second += char(b);
    }
    return out;
}

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// the rebuild request for tools/engine/mods_exefs.py: the exact writes and the bytes they replace
inline std::string request_json(const std::vector<File>& files, const Image& im) {
    std::string j = "{\"build_id\": \"" + std::string(g_bid) + "\", \"set\": \"" + set_hash(code_set(files)) + "\", \"patches\": [";
    bool first = true;
    for (auto& f : files) {
        if (f.kind != File::CODE) continue;
        for (auto& [off, bytes] : runs(f.code)) {
            j += first ? "\n  " : ",\n  "; first = false;
            j += "{\"module\": \"main\", \"offset\": " + std::to_string(off) + ", \"bytes\": \""
               + hex((const uint8_t*)bytes.data(), bytes.size()) + "\", \"expect\": \"" + hex(im.bytes + off, bytes.size())
               + "\"" + (f.slot < 0 ? std::string() : ", \"switch\": " + std::to_string(f.slot)) + ", \"from\": \"";
            for (char c : f.mod + "/" + f.rel) { if (c == '"' || c == '\\') j += '\\'; j += (unsigned char)c < ' ' ? '?' : c; }
            j += "\"}";
        }
    }
    return j + "\n]}\n";
}

// ---- Atmosphere cheat files: <mod>/cheats/<first 16 hex digits of the build id>.txt

struct Cheat {
    std::string name; bool master = false;
    std::vector<uint32_t> words;
    enum Kind { DATA, CODE, BAD } kind = DATA;
    Writes code;          // CODE: the instruction bytes it writes (joins the rebuild set)
    std::string note;     // BAD: why (an English text for pc_tr; %s takes arg)
    std::string arg;
};

inline bool cheat_file_name(const std::string& name) {   // "<16 hex>.txt", any case
    if (name.size() != 20) return false;
    std::string ext = name.substr(16);
    for (char& c : ext) c = lower(c);
    return ext == ".txt" && same_build(name.substr(0, 16));
}

// Atmosphere grammar: [name] / {master} headers, 8-hex-digit words; anything else ends the parse (entries so far stay)
inline std::vector<Cheat> parse_cheats(const std::string& s, std::string* stopped = nullptr) {
    std::vector<Cheat> out;
    size_t i = s.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (i < s.size()) {
        char ch = s[i];
        if ((unsigned char)ch <= ' ') { i++; continue; }
        if (ch == '[' || ch == '{') {
            size_t e = s.find(ch == '[' ? ']' : '}', i);
            if (e == std::string::npos) break;
            Cheat c; c.name = trim(s.substr(i + 1, e - i - 1)); c.master = ch == '{';
            out.push_back(c);
            i = e + 1; continue;
        }
        size_t j = i; while (j < s.size() && is_hex(s[j])) j++;
        if (j - i != 8 || out.empty()) { if (stopped) *stopped = "parse stopped at offset " + std::to_string(i); break; }
        out.back().words.push_back((uint32_t)strtoul(s.substr(i, 8).c_str(), nullptr, 16));
        i = j;
    }
    return out;
}

// words of the instruction at p[i] (the VM's skim in hle_cheat_vm.inc), 0 for an unknown opcode
inline size_t op_words(const std::vector<uint32_t>& p, size_t i) {
    uint32_t w = p[i], t = w >> 28, T = w >> 24 & 0xF;
    if (t >= 0xC) t = t << 4 | (w >> 24 & 0xF);
    if (t >= 0xF0) t = t << 4 | (w >> 20 & 0xF);
    auto vm = [](uint32_t T) -> size_t { return T == 8 ? 2 : (T == 1 || T == 2 || T == 4) ? 1 : 0; };
    switch (t) {
        case 0x0: case 0x1: return 2 + vm(T);
        case 0x2: return 1;
        case 0x3: return (w >> 24 & 0xF) ? 1 : 2;
        case 0x4: case 0x6: return 3;
        case 0x5: case 0x7: return 2;
        case 0x8: return 1;
        case 0x9: return 1 + ((w >> 8 & 0xF) ? vm(T) : 0);
        case 0xA: { uint32_t O = w >> 8 & 0xF; return (O == 2 || O == 4 || O == 5) ? 2 : 1; }
        case 0xC0: { uint32_t X = w >> 8 & 0xF, Tc = w >> 20 & 0xF; return 1 + ((X == 0 || X == 2) ? 1 : X == 4 ? vm(Tc) : 0); }
        case 0xC1: case 0xC2: case 0xC3: case 0xFF0: case 0xFF1: return 1;
        case 0xC4: return 3;
        case 0xFFF: { uint32_t X = w >> 8 & 0xF; return (X == 0 || X == 2) ? 2 : 1; }
    }
    return 0;
}

// a cheat writing main's code: one made only of plain stores (opcode 0) is a code patch that joins the rebuild set;
// mixed with anything else (a key, a condition, a loop) it is left out (the rival compiles those as switchable sites).
inline void classify_cheat(Cheat& c, const Image& im) {
    c.kind = Cheat::DATA; c.code.clear(); c.note.clear();
    bool only_stores = true, writes_code = false;
    Writes code;
    for (size_t i = 0; i < c.words.size();) {
        size_t n = op_words(c.words, i);
        if (!n || i + n > c.words.size()) {
            c.kind = Cheat::BAD;
            char b[32];
            if (n) { snprintf(b, sizeof b, "%zu", i); c.note = "It is cut short at word %s."; }
            else { snprintf(b, sizeof b, "%08X", c.words[i]); c.note = "It has an opcode the cheat engine does not know (%s)."; }
            c.arg = b; return;
        }
        uint32_t w = c.words[i], op = w >> 28, M = w >> 20 & 0xF;
        if ((op == 0 || op == 1 || (op == 5 && (w >> 12 & 0xF) == 0)) && (M == 2 || M == 3)) {
            c.kind = Cheat::BAD; c.note = "It uses the alias or ASLR memory region, which the port does not have."; return;
        }
        if (op == 0) {
            uint32_t T = w >> 24 & 0xF, R = w >> 16 & 0xF;
            uint64_t rel = (uint64_t)(w & 0xFF) << 32 | c.words[i + 1];
            if (M == 0 && R == 0 && rel < im.text_end) {   // (registers are 0 in a run without a register opcode)
                writes_code = true;
                uint64_t v = T == 8 ? (uint64_t)c.words[i + 2] << 32 | c.words[i + 3] : c.words[i + 2];
                for (uint32_t k = 0; k < T && k < 8; k++) code[(int64_t)(rel + k)] = uint8_t(v >> (k * 8));
            }
        } else only_stores = false;
        i += n;
    }
    if (!writes_code) return;
    if (!only_stores) { c.kind = Cheat::BAD; c.note = "It changes the game's code only under a condition (a button, a value), which the port cannot do yet."; return; }
    for (auto it = code.begin(); it != code.end();)
        it = it->first < im.size && im.bytes[it->first] == it->second ? code.erase(it) : std::next(it);
    c.kind = Cheat::CODE; c.code = code;
}

}   // namespace exefs
