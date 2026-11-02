// nxr_game.h: which game this engine build runs. A game is a guest image, a GEN_MAIN hook list and the runtime
// sources its games/<game>/game.py keeps; TotK is the default and its builds pass no game flag, so its command lines
// and objects stay as they were. Another game's build (mkninja_win.py --define) passes NXR_GAME_ID=<n>, leaves out
// the TotK-only sources (totk_*,...) and links runtime/game/none.cpp in their place. Generic code wraps what only
// TotK has (reads of TotK main addresses, its RomFS files, its game glue) in `#if NXR_GAME_TOTK`.
#pragma once

#define NXR_GAME_ID_TOTK 1   // (another game's number is its game.py GAME_ID: 2, 3, ...; no game is named here)

#ifndef NXR_GAME_ID
#define NXR_GAME_ID NXR_GAME_ID_TOTK
#endif
#define NXR_GAME_TOTK (NXR_GAME_ID == NXR_GAME_ID_TOTK)
