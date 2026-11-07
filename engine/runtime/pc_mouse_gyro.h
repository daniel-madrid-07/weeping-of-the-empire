// The mouse as the gyro: where a mouse count goes (pc_mouse.cpp's raw input) and how it turns the virtual controller
// (hle_hid.cpp mouse_gyro_feed). Pure, no Windows: tools/engine/test_pc_mouse_gyro.py tests it.
//
// #104 (Yveldi, mouse_gyro=always: "bow aim, vertical feels like a stick, horizontal raw"): while the mouse gyro aimed,
// each count turned the bow twice, through the gyro (linear on both axes) and through the camera's totals (the veneer:
// yaw linear, pitch shaped for the on-foot camera). With TOTK_MOUSE_GYRO_TOTALS=0 (opt-in: the gyro alone turns
// nothing with the game's Motion Controls off) a count reaches one path: the gyro while it aims, else the camera;
// pitch and yaw then follow the same law (k rad a count, about the controller's x and z).
#pragma once
#include <cmath>

namespace pcmg {

// The sinks of a relative count while captured: a PC scheme macro's (redirect), the stick / gyro counts (pc_mouse_take,
// pc_mouse_stick_counts), the camera's totals (the veneer). menu: a menu screen has the mouse; gyro_aim: the mouse gyro
// aims (pc_mouse_gyro_aim; only with TOTK_MOUSE_GYRO_TOTALS=0, else it stays false: both, as before).
enum : unsigned { SinkRedirect = 1, SinkCounts = 2, SinkTotals = 4 };
inline unsigned route(bool redirect, bool menu, bool gyro_aim) {
    if (redirect) return SinkRedirect;
    return SinkCounts | (menu || gyro_aim ? 0u : SinkTotals);
}

// The controller's angular velocity (rad/s, its own axes) for dx / dy counts over dt s: k = 0.002 x speed rad a count,
// right = yaw right (-z), down = pitch down (-x); the same k on both axes
inline void rate(double dx, double dy, double dt, double speed, double w[3]) {
    const double k = 0.002 * speed;
    w[0] = -dy * k / dt; w[1] = 0; w[2] = -dx * k / dt;
}

// R's rows: the world's fixed reference axes seen from the controller; turned by w over dt (r' = r - (w x r) dt,
// renormalised)
inline void turn(double R[3][3], const double w[3], double dt) {
    for (int r = 0; r < 3; r++) {
        double* v = R[r];
        const double cr[3] = {w[1] * v[2] - w[2] * v[1], w[2] * v[0] - w[0] * v[2], w[0] * v[1] - w[1] * v[0]};
        for (int i = 0; i < 3; i++) v[i] -= cr[i] * dt;
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; i++) v[i] /= n;
    }
}

}  // namespace pcmg
