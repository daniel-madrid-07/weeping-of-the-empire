// spvgen: direct GLSL -> SPIR-V 1.6 for the TotK translator's GLSL subset (same function body as glslang 15.1 / shaderc).
#pragma once
// SPVGEN_REV: bump when spvgen turns the same GLSL into different SPIR-V (shaders.c's cache/spirv key folds it in from
// 2 on, so players' cached modules from an older spvgen are not served). test_spvgen_rev.py fails when spvgen.c or this
// header changes and the hash below was not updated: then bump the revision if the output changed, and record the new
// hash either way.
#define SPVGEN_REV 1
// spvgen.c + spvgen.h sha256: eea1f099c8d64b979e85c5bf5f444b45afa42807db87d786ee14ee66cc31c72f
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Returns a malloc'd SPIR-V module (free() it) and its size in words, or NULL when the shader is outside the subset
// (*why = the reason, a static string); the caller then compiles it with shaderc. Thread-safe (state is thread-local).
uint32_t *spvgen(const char *src, size_t len, int fs, size_t *nwords, const char **why);
#ifdef __cplusplus
}
#endif
