#!/bin/bash
# Does the Linux runtime still build? (linux-port, 2026-11-02) The shared runtime gains Win32 calls with every landing;
# this compiles every Linux runtime object in tools/linux/env.sh's root and links them alone, so a change that breaks
# the Linux build is named the day it lands. Exit 0: every object compiled and nothing is undefined but what generated
# game code defines (f_* / s_* functions, g_*imp* tables); else the errors.
# usage (WSL, from a Windows checkout or worktree too): bash tools/linux/check.sh [--jobs N]
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
JOBS=4; [ "${1:-}" = --jobs ] && JOBS=$2
R=${NXR_LINUX:-$HOME/nxr-linux}
[ -d "$R/out/deps/vulkan-sdk" ] || { echo "no deps at $R/out/deps: tools/linux/env.sh fetch, apt; then build_deps.sh /out/deps in the root" >&2; exit 2; }
exec bash "$HERE/env.sh" run /bin/bash -c "
set -uo pipefail
B=/out/check; mkdir -p /out/check-nogen/gen/main
python3 /src/engine/tools/recomp/mkninja_linux.py --gen /out/check-nogen/gen/main --obj /out/check-nogen/obj --out \$B --deps /out/deps >/dev/null || exit 2
nice -n 10 ninja -C \$B -k 0 -j$JOBS objs-rt > \$B/compile.log 2>&1
failed=\$(grep -c '^FAILED' \$B/compile.log)
if [ \"\$failed\" != 0 ]; then
    echo \"linux check: \$failed runtime object(s) do not compile:\"
    grep -h '^/src.*error' \$B/compile.log | sed 's#/src/##' | sort -u | head -40
    exit 1
fi
cd \$B && find rt nvn -name '*.o' | sort > objs.txt
clang++ -fuse-ld=lld -no-pie -static-libstdc++ -static-libgcc -pthread -Wl,--error-limit=0 -o /dev/null @objs.txt \
    -L/out/deps/sdl3-install/lib -lSDL3 /out/deps/opus-install/lib/libopus.a /out/deps/vpx-install/lib/libvpx.a -lvulkan \
    /out/deps/vulkan-sdk/lib/libshaderc_combined.a -ldl -lpthread -lm 2> link.log
grep -o 'undefined symbol: .*' link.log | sed 's/undefined symbol: //' | sort -u | grep -v -E '^(f_|s_|g_imp_|g_s_imp_|g_ftab|g_s_ftab|g_sdk_)' > missing.txt
dup=\$(grep -c 'duplicate symbol' link.log)
if [ -s missing.txt ] || [ \"\$dup\" != 0 ]; then
    echo \"linux check: the runtime does not link: \$(wc -l < missing.txt) undefined, \$dup duplicate\"
    head -40 missing.txt; grep 'duplicate symbol' link.log | head -10
    exit 1
fi
echo \"linux check: \$(wc -l < objs.txt) runtime objects compile, nothing undefined but the game's own\"
"
