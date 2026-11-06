#!/bin/bash
# The Linux RomFS reader on a real install (linux-port 2026-11-04, linux-test D#83): tools/linux/test_romfs_source.cpp built
# in tools/linux/env.sh's root, then run on this Linux against an install's romfs-source.txt (written by Setup's step 1,
# extract --romfs-in-dump). It reads the game's first files through the dump; it prints paths and counts, never the key.
# usage (WSL / Linux, from a checkout or worktree): bash tools/linux/test_romfs_source.sh <install>/work/extracted/v100/romfs-source.txt
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:?the install\'s romfs-source.txt}
R=${NXR_LINUX:-$HOME/nxr-linux}
bash "$HERE/env.sh" run /bin/bash -c "clang++ -O2 -std=c++20 -Wall -I /src/engine/runtime /src/tools/linux/test_romfs_source.cpp \
    /src/engine/runtime/romfs_image.cpp -static-libstdc++ -static-libgcc -o /out/test_romfs_source"
"$R/out/test_romfs_source" "$SRC"
