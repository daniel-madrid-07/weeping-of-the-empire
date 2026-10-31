// Native replacements of hot guest functions (gen.py --hook).
//
// This tree carries none: each hook runs the recompiled original, f_<off>_orig. (A native replacement must leave the
// guest state exactly as the original does: return value, every register it writes, the same memory writes.)
#include "rt.h"

#ifdef _WIN32
void hot_native_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_704b7c_orig@@YAXPEIAUCtx@@@Z=?hot_native_orig_stub@@YAXPEIAUCtx@@@Z")
#define HOT_ORIG
#else
#define HOT_ORIG __attribute__((weak))
#endif

// f_704b7c: the submesh bounds vs view volume test of the shape draw loop
HOT_ORIG void f_704b7c_orig(Ctx *__restrict c);
void hook_f_704b7c(Ctx *__restrict c) { f_704b7c_orig(c); }
