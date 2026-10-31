// pc_restart.h: start the game again: F10's "Restart the game now" (pc_overlay.cpp) and the in-game skip into a new game
// (hle_hid.cpp skip_restart). Under the player launcher (launcher.c sets NXR_LAUNCHER_RESTART to a file it reads) the
// environment to add goes into that file and the process ends with PC_RESTART_CODE: the launcher starts the package again
// (a fresh log, its crash watch kept, no update offer in between). Without it (dev runs, boot.py): this exe again with
// the same command line, std handles and environment. Windows only.
#pragma once

#define PC_RESTART_CODE 0x52535452u   // "RSTR"

#ifdef __cplusplus
extern "C" {
#endif

// env: "KEY=VALUE" strings (an empty value removes KEY), NULL-terminated, or NULL. Returns only when the new process
// could not be started (the self-spawn's CreateProcess failed); else this process ends.
void pc_restart_process(const char *const *env);

#ifdef __cplusplus
}
#endif
