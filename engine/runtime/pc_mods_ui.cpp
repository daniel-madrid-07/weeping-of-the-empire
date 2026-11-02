// pc_mods_ui.cpp: the F10 "Mods" tab (pc_mods_ui.h). One row per mod, top first (the top wins a file two mods change):
// its switch, its name with what it serves (hle_fs counts them once the game has opened its files) or why it is not
// used, and up / down. Keyboard: Ctrl+Up / Ctrl+Down move the row in focus; pad: Y / X. Rows after totk-pc-recomp's
// Mods tab (runtime/ui/menu.cpp, MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// "Install a mod..." / "Install a mod folder...": Windows' file dialog, then mods_install.h (the launcher's too) puts the
// mod into this build's mods folder or the other version's by what its files name; a thread of its own does both (the
// game keeps drawing), and one line under the buttons says what happened.
#include "pc_mods_ui.h"

#include <stdio.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../third_party/imgui/imgui.h"
#include "mods.h"
#include "mods_scan.h"
#include "pc_text.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include "mods_install.h"
#endif

bool pc_mods_tab_available() { return !mods_folder().empty() || mods_read_only(); }

bool pc_mods_changed() { return mods_changed(); }

static const char *problem_text(const nxr_mod &m) {
    switch (m.problem) {
    case NXR_MOD_EMPTY: return pc_tr("This folder has no romfs, exefs or cheats files.");
    case NXR_MOD_BAD_NAME: return pc_tr("The folder name contains |, which the mod list uses between names. Rename the folder.");
    case NXR_MOD_OTHER_GAME: return pc_tr("It is made for another game (title %s).");
    case NXR_MOD_DUPLICATE: return pc_tr("An earlier mods folder has a mod with this name; this one is left out.");
    case NXR_MOD_PLUGIN: return pc_tr("It has code modules (subsdk or main), which the port cannot load, so the whole mod stays off.");
    case NXR_MOD_EXEFS_ONLY: return pc_tr("It only has code patches (exefs) or cheats, which this version cannot use yet.");
    case NXR_MOD_EXEFS: return m.problem_note.c_str();   // (mods_exefs_hook: already translated)
    default: return nullptr;
    }
}

bool pc_mods_open_folder() {
    const std::string dir = mods_folder();
    if (dir.empty()) return false;
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(mods_scan::from_u8(dir), ec);
    // (COM for the shell's extensions, as ShellExecute asks; a thread already in COM keeps its own mode)
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", mods_scan::from_u8(dir).c_str(), nullptr,
                                                            nullptr, SW_SHOWNORMAL)) > 32;
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
#else
    return false;
#endif
}

#ifdef _WIN32
static std::atomic<bool> g_installing{false};
static std::mutex g_install_mtx;
static std::string g_install_line;   // the last install's sentence (UTF-8, translated)

static std::string version_text(int v) {   // 143 -> "1.4.3"
    std::string d = std::to_string(v), out;
    for (size_t i = 0; i < d.size(); i++) { if (i) out += '.'; out += d[i]; }
    return out;
}
static std::string utf8(const wchar_t *w) {
    char b[1024];
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr) ? std::string(b) : std::string();
}
// the dialog, then the install, on its own thread
static void install_pick(bool folder, HWND owner) {
    if (g_installing.exchange(true)) return;
    std::thread([folder, owner] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring picked;
        IFileOpenDialog *d = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
            FILEOPENDIALOGOPTIONS o;
            if (SUCCEEDED(d->GetOptions(&o)))
                d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (folder ? FOS_PICKFOLDERS : 0));
            const std::string t = folder ? pc_tr("A mod's folder") : pc_tr("A mod (.zip, .7z or .rar)");
            const std::wstring wt = mods_scan::from_u8(t).wstring();
            d->SetTitle(wt.c_str());
            const COMDLG_FILTERSPEC spec[] = {{wt.c_str(), L"*.zip;*.7z;*.rar"}};
            if (!folder) d->SetFileTypes(1, spec);
            IShellItem *it = nullptr;
            PWSTR p = nullptr;
            if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                picked = p;
                CoTaskMemFree(p);
            }
            if (it) it->Release();
            d->Release();
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (picked.empty()) { g_installing = false; return; }
        // this build's mods folder and the other version's beside it (mods / mods-v143)
        const std::wstring first = mods_scan::from_u8(mods_folder()).wstring();
        const size_t sl = first.find_last_of(L"\\/");
        const std::wstring parent = first.substr(0, sl == std::wstring::npos ? 0 : sl);
        const bool later = first.size() > sl + 6 && _wcsnicmp(first.c_str() + sl + 1, L"mods-v", 6) == 0;
        const std::wstring d100 = later ? parent + L"\\mods" : first, d143 = later ? first : parent + L"\\mods-v143";
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
        const int in_use = TOTK_GAME_VERSION;
#else
        const int in_use = 100;
#endif
        ModInstall m;
        const int rc = mods_install(picked.c_str(), d100.c_str(), d143.c_str(), in_use, &m);
        printf("[mods] install %s: rc %d, kinds %d, version %d (%s), %s\n", utf8(picked.c_str()).c_str(), rc, m.kinds,
               m.version, m.named ? "named by its files" : "the one in use", rc == MI_OK ? utf8(m.dest).c_str() : "-");
        char line[1536];
        if (rc == MI_OK) {
            std::string what;
            if (m.kinds & MI_ROMFS) what += "RomFS";
            if (m.kinds & MI_EXEFS) what += std::string(what.empty() ? "" : " + ") + pc_tr("code patches");
            if (m.kinds & MI_CHEATS) what += std::string(what.empty() ? "" : " + ") + pc_tr("cheats");
            snprintf(line, sizeof line, pc_tr("Installed %s (%s, for %s) in %s. It loads at the next start."),
                     utf8(m.name).c_str(), what.c_str(), version_text(m.version).c_str(), utf8(m.dest).c_str());
            std::string s = line;
            if (!m.named) s += std::string(" ") + pc_tr("Its files don't name a game version: it was put with the one you play.");
            else if (m.version != 100 && m.version != 143) {
                snprintf(line, sizeof line, pc_tr("It is made for game version %s, which the port does not build: it was put "
                                                  "with the 1.4.3 mods and may not work."), version_text(m.version).c_str());
                s += std::string(" ") + line;
            }
            mods_need_restart("a mod installed");
            std::lock_guard<std::mutex> l(g_install_mtx);
            g_install_line = s;
        } else {
            // (each says what to do next: pc-ui's F10 style)
            const char *why = rc == MI_UNPACK ? pc_tr("Not installed: it could not be opened. Unpack it yourself and choose "
                                                      "its folder with Install a mod folder.")
                            : rc == MI_NOT_A_MOD ? pc_tr("Not installed: it has no romfs, exefs or cheats folder. If the mod is "
                                                         "packed inside it, unpack it and choose that folder instead.")
                            : rc == MI_OTHER_GAME ? pc_tr("Not installed: it is a mod for another game.")
                            : rc == MI_CODE_MODULES ? pc_tr("Not installed: it loads code of its own (subsdk / main), which a "
                                                            "recompiled game cannot load.")
                            : pc_tr("Not installed: it could not be copied. Check that the mods folder can be written "
                                    "(Open mods folder).");
            snprintf(line, sizeof line, "%s", why);
            std::lock_guard<std::mutex> l(g_install_mtx);
            g_install_line = line;
        }
        g_installing = false;
    }).detach();
}
#endif

void pc_mods_tab() {
    const std::vector<nxr_mod> mods = mods_list();
    const bool ro = mods_read_only();
    ImGui::TextWrapped("%s", pc_tr("Mods load at the next start. The top mod wins a file two mods change; the port's own "
                                   "files (PC button prompts, fixes) stay over a mod's."));
    if (ro) ImGui::TextDisabled("%s", pc_tr("The launcher chose these mods."));
    const std::string folder = mods_folder();
    if (!folder.empty()) {
        if (ImGui::Button(pc_tr("Open mods folder"))) pc_mods_open_folder();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", folder.c_str());
#ifdef _WIN32
        if (!ro) {
            ImGui::BeginDisabled(g_installing.load());
            if (ImGui::Button(pc_tr("Install a mod..."))) install_pick(false, GetForegroundWindow());
            ImGui::SameLine();
            if (ImGui::Button(pc_tr("Install a mod folder..."))) install_pick(true, GetForegroundWindow());
            ImGui::EndDisabled();
            std::lock_guard<std::mutex> l(g_install_mtx);
            if (g_installing.load()) {
                ImGui::TextDisabled("%s", pc_tr("Installing..."));
                ImGui::TextDisabled("%s", pc_tr("In exclusive fullscreen the game goes to the desktop while the dialog is open."));
            }
            else if (!g_install_line.empty()) ImGui::TextWrapped("%s", g_install_line.c_str());
        }
#endif
    }
    ImGui::Spacing();
    if (mods.empty()) {
        ImGui::TextWrapped("%s", pc_tr("No mods yet. Put each mod in a folder of its own in the mods folder (its romfs, "
                                       "exefs and cheats folders as emulators take them), then start the game again."));
        return;
    }
    if (!ro) ImGui::TextDisabled("%s", pc_tr("Ctrl+Up / Ctrl+Down (pad: Y / X) move the mod in focus."));
    static int focus = -1;   // the row that moved: its switch keeps the focus
    if (!ImGui::BeginTable("##mods", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("mod", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("order", ImGuiTableColumnFlags_WidthFixed);
    int move_from = -1, move_by = 0;
    for (int i = 0; i < (int)mods.size(); i++) {
        const nxr_mod &m = mods[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (focus == i) { ImGui::SetKeyboardFocusHere(); focus = -1; }
        bool on = m.on;
        ImGui::BeginDisabled(ro || m.problem == NXR_MOD_BAD_NAME || m.problem == NXR_MOD_DUPLICATE);
        if (ImGui::Checkbox("##on", &on)) mods_set_on(i, on);
        ImGui::EndDisabled();
        if (!ro && ImGui::IsItemFocused()) {
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) move_from = i, move_by = -1;
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) move_from = i, move_by = 1;
        }
        ImGui::TableSetColumnIndex(1);
        const char *why = problem_text(m);
        if (why || !m.on) ImGui::TextDisabled("%s", m.name.c_str());
        else ImGui::TextUnformatted(m.name.c_str());
        if (!m.collection.empty())   // a mod of a copied emulator collection: where it is ("yuzu/load/<title>/<mod>")
            ImGui::TextDisabled("%s", m.collection.c_str());
        if (why) {
            const ImVec4 warn(1.0f, 0.6f, 0.4f, 1.0f);
            if (m.problem == NXR_MOD_OTHER_GAME) ImGui::TextColored(warn, why, m.other_title.c_str());
            else ImGui::TextColored(warn, "%s", why);
        } else if (m.on && m.counted) {
            std::string line;
            char b[512];
            snprintf(b, sizeof b, pc_tr("%d files replaced, %d added"), m.replaced, m.added);
            line = b;
            if (m.by_port) snprintf(b, sizeof b, pc_tr("; %d left to the port's own files"), m.by_port), line += b;
            if (m.by_mod) snprintf(b, sizeof b, pc_tr("; %d left to %s, higher in the list"), m.by_mod, m.by_mod_name.c_str()), line += b;
            if (m.other_version) snprintf(b, sizeof b, pc_tr("; %d for another game version, not used"), m.other_version), line += b;
            ImGui::TextDisabled("%s", line.c_str());
        }
        if (!m.exefs_note.empty())   // the ExeFS patches' / cheats' state (mods_exefs_hook; already translated)
            ImGui::TextDisabled("%s", m.exefs_note.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(ro || i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move_from = i, move_by = -1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ro || i + 1 == (int)mods.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move_from = i, move_by = 1;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (move_from >= 0 && move_from + move_by >= 0 && move_from + move_by < (int)mods.size()) {
        mods_move(move_from, move_by);
        focus = move_from + move_by;
    }
}
