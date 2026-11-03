// TotK Linux port: the MSVC CRT names the runtime calls without <windows.h> (pc_dlss.cpp _putenv_s, totk_rsdb_memo.cpp
// _stricmp, textures.c strlcpy, ...). Force-included into every runtime TU by mkninja_linux.py, and by windows.h.
#pragma once
#ifndef _WIN32
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _strnicmp strncasecmp
#define _stricmp strcasecmp
static inline void *_aligned_malloc(size_t size, size_t align) { void *p = NULL; if (align < sizeof(void *)) align = sizeof(void *); return posix_memalign(&p, align, size) ? NULL : p; }
static inline void _aligned_free(void *p) { free(p); }
// _putenv_s: "" removes the variable, as on Windows
static inline int _putenv_s(const char *k, const char *v) { return (v && *v ? setenv(k, v, 1) : unsetenv(k)) ? errno : 0; }
#define YieldProcessor() __builtin_ia32_pause()
#ifndef MAX_PATH
#define MAX_PATH 4096   // (PATH_MAX: Linux paths are not cut at Windows' 260)
#endif
// strlcpy is in glibc only from 2.38 (the Linux build's baseline is 2.35): ours, under its own name so a newer libc's
// declaration never clashes
static inline size_t rt_strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
#define strlcpy rt_strlcpy

#ifdef __cplusplus
}
#endif
#endif   // !_WIN32
