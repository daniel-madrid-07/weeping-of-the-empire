// pc_imgui.cpp: Dear ImGui 1.93.0 (third_party/imgui, MIT) compiled as one runtime TU, so the build's runtime/*.cpp
// glob picks it up without new build rules (tools/recomp/mkninja_win.py is part of the shared game-object key).
// No backends: input is pc_overlay.cpp's, rendering pc_overlay_vk.c's.
#include "../third_party/imgui/imgui.cpp"
#include "../third_party/imgui/imgui_draw.cpp"
#include "../third_party/imgui/imgui_tables.cpp"
#include "../third_party/imgui/imgui_widgets.cpp"
