// pc_cheats_ui.cpp: the F10 "Cheats" tab (pc_cheats_ui.h). One group per mod (its cheat file), one switch per cheat;
// a cheat the port cannot run is greyed with why. Rows after totk-pc-recomp's runtime/ui/cheats_tab.cpp (MIT,
// Copyright (c) 2026 Valiantsin Dzerakh).
#include "pc_cheats_ui.h"

#include <string>
#include <vector>

#include "../third_party/imgui/imgui.h"
#include "mods_exefs.h"
#include "pc_text.h"

bool pc_cheats_tab_available() { return !mods_cheats().empty(); }

void pc_cheats_tab() {
    const std::vector<nxr_cheat> cheats = mods_cheats();
    ImGui::TextWrapped("%s", pc_tr("Cheats from your mods' cheats folders (Atmosphere format). A cheat that changes values "
                                   "works at once; one that changes the game's code applies when the game starts again. "
                                   "Cheats can break a save: keep a copy."));
    ImGui::Spacing();
    std::string mod;
    for (int i = 0; i < (int)cheats.size(); i++) {
        const nxr_cheat &c = cheats[i];
        if (c.mod != mod) {
            mod = c.mod;
            ImGui::Spacing();
            ImGui::SeparatorText(mod.c_str());
        }
        ImGui::PushID(i);
        bool on = c.on;
        ImGui::BeginDisabled(!c.usable);
        if (ImGui::Checkbox(c.name.empty() ? "?" : c.name.c_str(), &on)) mods_cheat_set(i, on);
        ImGui::EndDisabled();
        if (!c.note.empty()) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", c.note.c_str());
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}
