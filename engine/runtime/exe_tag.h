// The game key and patch set this totk.exe was linked against (build.py's tag at the file's end, exe_tag.cpp,
// tools/engine/exekey.py). Callable from C and C++, from any thread, and before main: the first call reads the file.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// The 16-hex game key, or NULL: an exe linked before the tag existed, or not by build.py.
const char *totk_link_key(void);
// The key's game-patch features joined by '+' (its own <img>/patches.json, e.g. "fps60+realtime+...+cascade"); ""
// when the key was built without patches (an unpatched dev key); NULL when the tag records none (an untagged exe, one
// tagged before the patches field, or a patched key whose patches.json could not be read).
const char *totk_link_patches(void);
// main(), early: logs the tag ('[build] linked against key ...') and compares it with player.txt's key= next to the
// exe; a mismatch logs '[build] WARNING' lines and changes exe_tag_title(). No player.txt (a dev boot): no compare.
void exe_tag_check(void);
// The window title: "Tears of the Kingdom", or with "- WRONG BUILD: exe key ..., package key ..." after a mismatch.
const char *exe_tag_title(void);
#ifdef __cplusplus
}
#endif
