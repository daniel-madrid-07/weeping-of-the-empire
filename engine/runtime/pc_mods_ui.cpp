// pc_mods_ui.cpp: the F10 "Mods" tab (pc_mods_ui.h). One row per mod, top first (the top wins a file two mods change):
// its switch, its name with what it serves (hle_fs counts them once the game has opened its files) or why it is not
// used, and up / down. Keyboard: Ctrl+Up / Ctrl+Down move the row in focus; pad: Y / X. Rows after totk-pc-recomp's
// Mods tab (runtime/ui/menu.cpp, MIT, Copyright (c) 2026 Valiantsin Dzerakh).
#include "pc_mods_ui.h"

#include <stdio.h>

#include <string>
#include <vector>

#include "../third_party/imgui/imgui.h"
#include "mods.h"
#include "mods_scan.h"
#include "pc_text.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
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

static void open_folder(const std::string &dir) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(mods_scan::from_u8(dir), ec);
    ShellExecuteW(nullptr, L"open", mods_scan::from_u8(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)dir;
#endif
}

void pc_mods_tab() {
    const std::vector<nxr_mod> mods = mods_list();
    const bool ro = mods_read_only();
    ImGui::TextWrapped("%s", pc_tr("Mods load at the next start. The top mod wins a file two mods change; the port's own "
                                   "files (PC button prompts, fixes) stay over a mod's."));
    if (ro) ImGui::TextDisabled("%s", pc_tr("The launcher chose these mods."));
    const std::string folder = mods_folder();
    if (!folder.empty()) {
        if (ImGui::Button(pc_tr("Open mods folder"))) open_folder(folder);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", folder.c_str());
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
