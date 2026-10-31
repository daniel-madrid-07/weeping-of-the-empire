// pc_bind_ui.cpp: the F10 window's "Keys" tab, built from pc_bind_pages.inc (pc_bind_ui.h). One row per PC_PAGE entry,
// in the file's order, its page a section; PC_PAGE_FIXED lines are not shown. A row shows the keys pc_bind_get gives
// at the entry's scope; "Change" captures the next key, wheel notch or mouse button for slot 0 (the prompt's key),
// "Add" for the next free slot, "Default" drops the scope's override (pc_bind_reset). A write that swaps another
// action's key, or that pc_bind_set refuses, says so on the row for a few seconds. "Restore defaults" resets every scope
// the entries use. Present thread only (ImGui), like the rest of the window.
#include "pc_bind_ui.h"

#include "pc_input.h"
#include "pc_lang.h"
#include "pc_text.h"
#include "../third_party/imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// The scope tokens of pc_bind_pages.inc: the four groups, then every context by its PcCtx name (a token the file
// starts using that is not here fails to compile)
namespace page_scope {
enum : int {
    ROOT = PC_SCOPE_ROOT, GAMEPLAY = PC_SCOPE_GAMEPLAY, UI = PC_SCOPE_UI, MENUS = PC_SCOPE_MENUS,
    WORLD = PCX_WORLD, OVERLAY = PCX_OVERLAY, TITLE = PCX_TITLE, SAVES = PCX_SAVES, PAUSE = PCX_PAUSE,
    POPUP = PCX_POPUP, PAD = PCX_PAD, MAP = PCX_MAP, OPTIONS = PCX_OPTIONS, SHOP = PCX_SHOP, MENU = PCX_MENU,
    DIALOGUE = PCX_DIALOGUE, CHOICE = PCX_CHOICE, CUTSCENE = PCX_CUTSCENE, TRANSITION = PCX_TRANSITION,
    CLIMB = PCX_CLIMB, AIR = PCX_AIR, GLIDE = PCX_GLIDE, SWIM = PCX_SWIM, AIM = PCX_AIM, ABILITY = PCX_ABILITY,
    HORSE = PCX_HORSE, VEHICLE = PCX_VEHICLE, LOADING = PCX_LOADING, QUICKMENU = PCX_QUICKMENU, PADLOG = PCX_PADLOG,
    CAMERA = PCX_CAMERA, SCOPE = PCX_SCOPE, CONSTRUCT = PCX_CONSTRUCT, RECIPE = PCX_RECIPE,
};
}  // namespace page_scope

const PcPageEntry kEntries[] = {
#define PC_PAGE(page, scope, action, label) {page, page_scope::scope, action, label},
#include "pc_bind_pages.inc"
#undef PC_PAGE
};
constexpr int kCount = sizeof kEntries / sizeof kEntries[0];

const char *err_text(int e) {
    switch (e) {
    case PCB_NOT_READY: return pc_tr("Rebinding is not available yet");
    case PCB_UNKNOWN: return pc_tr("No such action here");
    case PCB_FIXED: return pc_tr("A fixed control has that key");
    case PCB_RESERVED: return pc_tr("Reserved key (F10, F12, Windows, Print Screen)");
    case PCB_LAST_KEY: return pc_tr("Confirm, back and pause must keep a key");
    case PCB_CHAIN: return pc_tr("That swap would clash again: choose another key");
    case PCB_KIND: return pc_tr("The wheel only works for single presses");
    default: return pc_tr("Refused");
    }
}

// The name of an input for the player (pc_key_name), in the overlay's language (Spanish or English: pc_lang.cpp)
std::string input_name(int input) {
    const char *n = pc_key_name(input, pc_lang_es() ? "es" : "en");
    if (n && *n) return n;
    char b[16];
    snprintf(b, sizeof b, "#%d", input);
    return b;
}

// An action's label as a page gives it (a swap names the other action), else its name
const char *label_of(int scope, const char *action) {
    for (const PcPageEntry &e : kEntries)
        if (e.scope == scope && !strcmp(e.action, action)) return pc_tr(e.label);
    for (const PcPageEntry &e : kEntries)
        if (!strcmp(e.action, action)) return pc_tr(e.label);
    return action;
}

int g_cap = -1;      // the entry waiting for a key
int g_cap_slot = 0;  // the slot it writes
int g_note = -1;     // the entry showing a note (a refusal, the swaps), until g_note_until
bool g_note_err = false;
double g_note_until = 0;
std::string g_note_text;

void note(int i, bool err, std::string text) {
    g_note = i;
    g_note_err = err;
    g_note_text = std::move(text);
    g_note_until = ImGui::GetTime() + 4.0;
}

void start_capture(int i, int slot) {
    if (!pc_input_capture_begin(PCC_WHEEL)) return note(i, true, err_text(PCB_NOT_READY));
    printf("[pc-keys] %s (scope %d) slot %d: waiting for a key\n", kEntries[i].action, kEntries[i].scope, slot);
    g_cap = i;
    g_cap_slot = slot;
}

void bind(int i, int slot, int input) {
    const PcPageEntry &e = kEntries[i];
    PcBindChange ch[8];
    const int n = pc_bind_set(e.scope, e.action, slot, input, ch, 8);
    printf("[pc-keys] %s (scope %d) slot %d <- %s: %d\n", e.action, e.scope, slot, input_name(input).c_str(), n);
    if (n < 0) return note(i, true, err_text(n));
    std::string swapped;
    for (int k = 1; k < n && k < 8; k++) {
        swapped += swapped.empty() ? pc_tr("Swapped with ") : ", ";
        swapped += label_of(ch[k].scope, ch[k].action);
    }
    if (!swapped.empty()) note(i, false, swapped);
}

// Once a frame while the tab is drawn
void poll() {
    if (g_cap < 0) return;
    const int r = pc_input_capture_poll();
    if (r == 0) return;
    const int i = g_cap;
    g_cap = -1;
    if (r > 0) bind(i, g_cap_slot, r);  // (r < 0: Esc or the focus went: nothing changes)
    else printf("[pc-keys] %s: capture cancelled\n", kEntries[i].action);
}

void row(int i) {
    const PcPageEntry &e = kEntries[i];
    int in[4];
    unsigned flags = 0;
    const int n = pc_bind_get(e.scope, e.action, in, 4, &flags);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (flags & PCB_F_INACTIVE) {
        ImGui::TextDisabled("%s", pc_tr(e.label));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pc_tr("Stored now; acts once the PC gameplay keys are on (Controls tab)."));
    } else {
        ImGui::TextUnformatted(pc_tr(e.label));
    }
    ImGui::TableNextColumn();
    ImGui::PushID(i);
    if (g_cap == i) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("Press a key, mouse button or wheel (Esc: cancel)"));
    } else {
        std::string keys;
        for (int k = 0; k < n; k++) keys += (k ? ", " : "") + input_name(in[k]);
        if (n < 0) ImGui::TextDisabled("-");
        else if (n == 0) ImGui::TextDisabled("(none)");
        else ImGui::TextUnformatted(keys.c_str());
        ImGui::BeginDisabled(n < 0 || g_cap >= 0 || (flags & PCB_F_FIXED));
        ImGui::SameLine();
        if (ImGui::SmallButton(pc_tr("Change"))) start_capture(i, 0);
        if (n < 4) {
            ImGui::SameLine();
            if (ImGui::SmallButton(pc_tr("Add"))) start_capture(i, n > 0 ? n : 0);
        }
        if (flags & PCB_F_OVERRIDDEN) {
            ImGui::SameLine();
            if (ImGui::SmallButton(pc_tr("Default"))) {
                PcBindChange ch[8];
                const int r = pc_bind_reset(e.scope, e.action, ch, 8);
                printf("[pc-keys] %s (scope %d) default: %d\n", e.action, e.scope, r);
                if (r < 0) note(i, true, err_text(r));
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::TableNextColumn();
    if (g_note == i && ImGui::GetTime() < g_note_until) {
        if (g_note_err) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", g_note_text.c_str());
        else ImGui::TextDisabled("%s", g_note_text.c_str());
    } else if (flags & PCB_F_OVERRIDDEN) {
        ImGui::TextDisabled("changed");
    }
    ImGui::PopID();
}

}  // namespace

const PcPageEntry *pc_bind_page_entries(int *count) {
    if (count) *count = kCount;
    return kEntries;
}

const PcPageEntry *pc_bind_page_find(const char *action) {
    for (const PcPageEntry &e : kEntries)
        if (!strcmp(e.action, action)) return &e;
    return nullptr;
}

bool pc_bind_keys_available() {
    static const bool on = [] {
        const char *e = getenv("TOTK_OPTIONS_KEYS");
        if (e && *e) return strcmp(e, "0") != 0;
        int in[4];
        unsigned flags = 0;
        return pc_bind_get(kEntries[0].scope, kEntries[0].action, in, 4, &flags) != PCB_NOT_READY;
    }();
    return on;
}

void pc_bind_keys_closed() {
    if (g_cap < 0) return;
    g_cap = -1;
    pc_input_capture_cancel();
}

void pc_bind_keys_tab() {
    poll();
    int probe[4];
    unsigned pf = 0;
    if (pc_bind_get(kEntries[0].scope, kEntries[0].action, probe, 4, &pf) == PCB_NOT_READY)
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("Key rebinding is not available in this build yet."));
    const char *page = nullptr;
    bool open = false;
    for (int i = 0; i < kCount; i++) {
        if (!page || strcmp(page, kEntries[i].page)) {  // a new page: its section
            if (open) ImGui::EndTable();
            page = kEntries[i].page;
            ImGui::SeparatorText(pc_tr(page));
            open = ImGui::BeginTable(page, 3, ImGuiTableFlags_SizingStretchProp);
            if (open) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 0.44f);
                ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            }
        }
        if (open) row(i);
    }
    if (open) ImGui::EndTable();
    ImGui::Spacing();
    ImGui::BeginDisabled(g_cap >= 0);
    if (ImGui::Button(pc_tr("Restore defaults"))) {  // every scope the entries use, once
        std::vector<int> done;
        for (const PcPageEntry &e : kEntries) {
            bool seen = false;
            for (int s : done) seen |= s == e.scope;
            if (seen) continue;
            done.push_back(e.scope);
            static PcBindChange ch[256];  // a whole scope's overrides and the swaps they undo
            const int r = pc_bind_reset(e.scope, nullptr, ch, 256);
            printf("[pc-keys] restore defaults, scope %d: %d\n", e.scope, r);
        }
    }
    ImGui::EndDisabled();
}
