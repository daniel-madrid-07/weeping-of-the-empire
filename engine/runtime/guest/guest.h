// guest/guest.h for the engine: the hook calls the C++ instead of f_<off>_orig.
//   GUEST_FN(name, 1000abc120, R(A...)) integer, bool and pointer arguments (up to 8) and result
//   guest_mem::guest_data<T>(0x1004557b20)          a guest global
// Anything else (vcall, call_ptr, Sret, float or aggregate arguments) does not compile here. The engine finds this
// file first for "guest/guest.h" (-I engine/runtime); the source sits in the build's snapshot (SNAPSHOT_PATHS). A
// hook runs the C++ inside guest_bridge::Enter e(c): guest calls run on that Ctx, on the guest stack below the
// hooked function's caller, and x30 is the caller's again when the hook returns. The callees keep x19-x29 and
// d8-d15 (AAPCS64) and the C++ never touches them, so the hook contract holds.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "guest/types.h"
#include "rt.h"
#include "totk_version.h"

namespace guest_bridge {

constexpr u64 kGhidraMain = 0x1000000000ull;
constexpr u64 kEngineMain = 0x7100000000ull;  // ... and in the engine (gen.py IMGBASE)
// totk_version.h: a 1.0.0 offset becomes the built game version's (the identity on 1.0.0)
constexpr u64 rebase(u64 addr) { return totk_main_off(addr - kGhidraMain) + kEngineMain; }

inline thread_local Ctx *tl_ctx = nullptr;

struct Enter {
    Ctx *c, *prev;
    u64 lr;
    explicit Enter(Ctx *ctx) : c(ctx), prev(tl_ctx), lr(ctx->x[30]) { tl_ctx = ctx; }
    ~Enter() {
        c->x[30] = lr;
        tl_ctx = prev;
    }
    Enter(const Enter &) = delete;
    Enter &operator=(const Enter &) = delete;
};

template <class T> inline u64 to_reg(T v) {
    if constexpr (std::is_pointer_v<T>) return static_cast<u64>(reinterpret_cast<uintptr_t>(v));
    else if constexpr (std::is_same_v<T, bool>) return v ? 1 : 0;
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return static_cast<u64>(static_cast<s64>(v));
    else if constexpr (std::is_integral_v<T>) return static_cast<u64>(v);
    else static_assert(sizeof(T) == 0, "the engine's guest bridge passes integers, bools and pointers only");
}

template <class R> inline R from_reg(u64 v) {
    if constexpr (std::is_pointer_v<R>) return reinterpret_cast<R>(static_cast<uintptr_t>(v));
    else if constexpr (std::is_same_v<R, bool>) return static_cast<u8>(v) != 0;  // AAPCS64: the low byte
    else if constexpr (std::is_integral_v<R>) return static_cast<R>(v);           // narrow types: the low bits
    else static_assert(sizeof(R) == 0, "the engine's guest bridge returns integers, bools and pointers only");
}

template <u64 Target, class Sig> struct GuestFunc;
template <u64 Target, class R, class... A> struct GuestFunc<Target, R(A...)> {
    static_assert(sizeof...(A) <= 8, "stack arguments are not bridged");
    R operator()(A... a) const {
        Ctx *const c = tl_ctx;
        if (!c) {
            fprintf(stderr, "[guest-bridge] guest call to %#llx outside a hook\n", (unsigned long long)Target);
            abort();
        }
        const u64 regs[sizeof...(A) + 1] = {to_reg<A>(a)..., 0};
        for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = regs[i];
        call_ind(c, Target);
        if constexpr (!std::is_void_v<R>) return from_reg<R>(c->x[0]);
    }
};

}  // namespace guest_bridge

namespace guest_mem {
template <class T> inline T *guest_data(u64 addr) {
    return reinterpret_cast<T *>(static_cast<uintptr_t>(guest_bridge::rebase(addr)));
}
}  // namespace guest_mem

#define GUEST_FN(name, hex, sig) \
    inline constexpr ::guest_bridge::GuestFunc<::guest_bridge::rebase(0x##hex##ULL), sig> name {}
