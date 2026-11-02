// present_color.h: the colour adjustment's factors from its settings (exec.c renderer_apply_color, display.color rows).
// Header-only so tools/engine/present_color_test.c checks them without the renderer.
#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline float present_color_num(const char *v, float def, float lo, float hi) {
    if (!v || !*v) return def;
    float f = (float)atof(v);
    return f < lo ? lo : f > hi ? hi : f;
}
// k = brightness (added), contrast, saturation, gamma (present_color.frag). preset: "" / NULL / anything unknown = off,
// "vivid", "bright", or "custom" (the four values, each clamped to its row's range; missing = the game's). Returns false
// for the identity, whatever the preset: then the present draws the plain present.frag.
static inline bool present_color_factors(const char *preset, const char *bright, const char *contrast, const char *sat,
                                         const char *gamma, float k[4]) {
    k[0] = 0, k[1] = 1, k[2] = 1, k[3] = 1;
    const char *p = preset ? preset : "";
    if (!strcmp(p, "vivid")) k[1] = 1.06f, k[2] = 1.25f;   // livelier colours, a little more contrast
    else if (!strcmp(p, "bright")) k[3] = 0.85f;            // dark tones lifted (caves, the Depths, night)
    else if (!strcmp(p, "custom")) {
        k[0] = present_color_num(bright, 0, -0.3f, 0.3f), k[1] = present_color_num(contrast, 1, 0.7f, 1.5f);
        k[2] = present_color_num(sat, 1, 0.5f, 2.0f), k[3] = present_color_num(gamma, 1, 0.8f, 1.2f);
    }
    return k[0] != 0 || k[1] != 1 || k[2] != 1 || k[3] != 1;
}
