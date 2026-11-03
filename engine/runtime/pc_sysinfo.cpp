// pc_sysinfo.cpp: F10's "Copy system info" text never names the player: their profile folder becomes %USERPROFILE%
// and their user name <user>, matched case-insensitively in UTF-16 (CharLowerBuffW), so a name like José, Müller
// or a Cyrillic / CJK one is found too. Windows only.
#ifdef _WIN32
#include "pc_sysinfo.h"

#include <windows.h>

namespace {
std::wstring wide(const std::string &s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string utf8(const std::wstring &w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());   // (same length: UTF-16 units map one to one)
    return s;
}
// every case-insensitive `what` in s replaced by `with`
void replace_ci(std::wstring &s, const std::wstring &what, const std::wstring &with) {
    if (what.empty()) return;
    std::wstring low = lower(s);
    const std::wstring w = lower(what), lw = lower(with);
    for (size_t at = 0; (at = low.find(w, at)) != std::wstring::npos; at += with.size()) {
        s.replace(at, what.size(), with);
        low.replace(at, what.size(), lw);
    }
}
std::wstring env(const wchar_t *name) {
    wchar_t v[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, v, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(v, n) : std::wstring();
}
}  // namespace

std::string pc_mask_user(const std::string &text, const std::wstring &profile, const std::wstring &user) {
    std::wstring t = wide(text);
    if (profile.size() >= 3) replace_ci(t, profile, L"%USERPROFILE%");
    if (user.size() >= 3) replace_ci(t, user, L"<user>");   // (a shorter name would hit ordinary words)
    return utf8(t);
}

std::string pc_mask_user(const std::string &text) { return pc_mask_user(text, env(L"USERPROFILE"), env(L"USERNAME")); }
#else
// Linux: the home folder becomes ~ and the user name <user> (a path is matched as is: Linux paths are case-sensitive)
#include "pc_sysinfo.h"
#include <stdlib.h>
#include <string.h>
namespace {
void replace_all(std::string &s, const std::string &what, const std::string &with) {
    for (size_t at = 0; !what.empty() && (at = s.find(what, at)) != std::string::npos; at += with.size()) s.replace(at, what.size(), with);
}
}  // namespace
std::string pc_mask_user(const std::string &text) {
    std::string t = text;
    const char *home = getenv("HOME"), *user = getenv("USER");
    if (home && strlen(home) >= 3) replace_all(t, home, "~");
    if (user && strlen(user) >= 3) replace_all(t, user, "<user>");   // (a shorter name would hit ordinary words)
    return t;
}
#endif
