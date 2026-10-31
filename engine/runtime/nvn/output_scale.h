// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render-target scale (relative to the game's 1920x1080 sizes)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
} OutScale;

// dw x dh: the window's drawable size in pixels; now: seconds; gpu_ms: GPU time of the last frame (< 0: unknown)
void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o);
double output_scale_dynamic_factor(void);
// The presented image's own aspect, crop included (device.c backend_present, each present), and the fit that places an
// image of that aspect in the window over output_scale's 16:9 rect (the 21:9 / 32:9 canvases). device.c draws with it
// and the menu cursor maps the pointer through it; a 16:9 or unknown aspect leaves *o as it is.
void output_set_image_aspect(double a);
void output_fit_image(double dw, double dh, OutScale *o);
