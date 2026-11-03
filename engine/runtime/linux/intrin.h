// TotK Linux port: the MSVC <intrin.h> names the runtime uses (pc_overlay.cpp __cpuid), found before clang's own intrin.h
// (which only forwards to MSVC's) because runtime/linux is first on the Linux include path (mkninja_linux.py)
#pragma once
#ifndef _WIN32
#include <x86intrin.h>
static inline void __cpuidex(int r[4], int leaf, int sub) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static inline void __cpuid(int r[4], int leaf) { __cpuidex(r, leaf, 0); }
#endif
