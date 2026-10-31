#pragma once

#include <cstddef>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using uintptr = std::uintptr_t;

// Layout checks against the guest (AArch64 LP64: same sizes and alignments as x86-64 for these types).
#define GUEST_OFFSET(T, member, off) static_assert(offsetof(T, member) == (off), #T "::" #member " offset")
#define GUEST_SIZE(T, size) static_assert(sizeof(T) == (size), "sizeof(" #T ")")
