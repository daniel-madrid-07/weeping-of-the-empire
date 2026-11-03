#!/bin/bash
# The Linux SDK a player's Setup builds the game with (linux-port, 2026-11-02): everything but the compiler, in one
# archive, made inside tools/linux/env.sh's Ubuntu 22.04 root:
#   sysroot/  glibc 2.35's headers, crt objects and link-time libraries (libc, libm, libmvec, the loader), the Linux
#             kernel's user headers, GCC 12's libstdc++ / libgcc (static: the game links them in) and libvulkan's link
#             name. clang --sysroot builds against it, so the exe runs on any glibc >= 2.35 whatever the player's PC
#             has installed (SteamOS has no headers at all).
#   deps/     engine/tools/linux/build_deps.sh's prefix: SDL3 (shared, shipped next to the game), Opus, libvpx, the
#             Vulkan SDK's headers and shaderc.
# usage (in the root): bash /src/tools/linux/sdk.sh /out/deps /out/sdk   ->  /out/sdk.tar.zst (+ .sha256)
# Licences: glibc LGPL-2.1+ (crt objects: their linking exception), linux-libc-dev GPL-2.0 WITH Linux-syscall-note,
# libstdc++ / libgcc GPL-3.0 WITH GCC-exception-3.1, Vulkan loader Apache-2.0, SDL3 zlib, Opus / libvpx BSD-3-Clause,
# shaderc Apache-2.0 (+ glslang BSD-3-Clause / SPIRV-Tools Apache-2.0); each package's copyright file goes in licenses/.
set -euo pipefail
DEPS=${1:?deps dir}; OUT=${2:?out dir}
[ -n "${NXR_IN_ROOT:-}" ] || { echo "run inside tools/linux/env.sh's root" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/sysroot" "$OUT/deps" "$OUT/licenses"
S=$OUT/sysroot
GCC=/usr/lib/gcc/x86_64-linux-gnu/12
MA=x86_64-linux-gnu

copy() { for p in "$@"; do mkdir -p "$S$(dirname "$p")"; cp -a "$p" "$S$p"; done; }
# headers: glibc's and the kernel's (every top-level header and the libc / kernel directories, not the -dev packages of
# X11 / Wayland / PipeWire the root also holds: nothing of ours includes them), libstdc++'s
copy /usr/include/*.h
for d in arpa asm-generic linux misc mtd net netash netatalk netax25 neteconet netinet netipx netiucv netpacket netrom \
         netrose nfs protocols rdma rpc scsi sound sys video xen c++ "$MA"; do
    [ -e "/usr/include/$d" ] && copy "/usr/include/$d"
done
# link-time files (the .so linker scripts name /lib/$MA/libc.so.6 and /lib64/ld-linux-x86-64.so.2: lld prefixes them
# with the sysroot)
copy /usr/lib/$MA/crt1.o /usr/lib/$MA/crti.o /usr/lib/$MA/crtn.o /usr/lib/$MA/Scrt1.o /usr/lib/$MA/libc.so \
     /usr/lib/$MA/libc_nonshared.a /usr/lib/$MA/libm.so /usr/lib/$MA/libmvec.so /usr/lib/$MA/libpthread.a \
     /usr/lib/$MA/libdl.a /usr/lib/$MA/librt.a /usr/lib/$MA/libutil.a
for f in libc.so.6 libm.so.6 libmvec.so.1 ld-linux-x86-64.so.2 libvulkan.so.1; do
    mkdir -p "$S/lib/$MA"; cp -L "/usr/lib/$MA/$f" "$S/lib/$MA/$f"
done
mkdir -p "$S/lib64" "$S/usr/lib/$MA"
ln -sf ../lib/$MA/ld-linux-x86-64.so.2 "$S/lib64/ld-linux-x86-64.so.2"
ln -sf ../../../lib/$MA/libvulkan.so.1 "$S/usr/lib/$MA/libvulkan.so"
# (Ubuntu's merged /usr: the root's /lib is usr/lib; in the sysroot the two are separate folders, as the scripts name them)
copy $GCC/crtbegin.o $GCC/crtbeginS.o $GCC/crtbeginT.o $GCC/crtend.o $GCC/crtendS.o $GCC/libgcc.a $GCC/libgcc_eh.a \
     $GCC/libstdc++.a $GCC/libsupc++.a
# every absolute symlink made relative within the sysroot (Ubuntu's libmvec.so -> /lib/...: an unpacker refuses links
# out of its folder, and a link must not reach the player's own system), or the file itself where its target is not here
find "$S" -type l | while read -r l; do
    t=$(readlink "$l")
    case "$t" in
        /*) if [ -e "$S$t" ]; then ln -sfr "$S$t" "$l"; else rm "$l"; cp -L "$t" "$l"; fi ;;
    esac
done
# deps: what the build reads (headers, static libraries, SDL3's shared library), not the sources
cp -a "$DEPS/sdl3-install" "$DEPS/opus-install" "$DEPS/vpx-install" "$DEPS/vulkan-sdk" "$OUT/deps/"
rm -rf "$OUT/deps/sdl3-install/lib/cmake" "$OUT/deps/sdl3-install/lib/pkgconfig" "$OUT/deps/sdl3-install/share" \
       "$OUT/deps/vpx-install/lib/pkgconfig" "$OUT/deps/opus-install/lib/pkgconfig"
mkdir -p "$OUT/deps/vulkan-headers" && ln -s ../vulkan-sdk/include "$OUT/deps/vulkan-headers/include"
# licences: the Debian copyright files of the packages the sysroot comes from, and each dependency's own
for pkg in libc6-dev linux-libc-dev libstdc++-12-dev libgcc-12-dev libvulkan1; do
    cp "/usr/share/doc/$pkg/copyright" "$OUT/licenses/$pkg.copyright" 2>/dev/null || true
done
# where the exact sources of those binaries are (LGPL / GPL: glibc, libstdc++, libgcc, the kernel headers): each package's
# Ubuntu source package and version, from its Debian changelog (env.sh unpacks the .debs with tar: no dpkg record)
{
    echo "The sysroot's binaries are Ubuntu 22.04 (jammy) packages, unmodified. Their exact sources: the Ubuntu source"
    echo "packages below ('apt-get source <source>=<version>', or https://launchpad.net/ubuntu/+source/<source>/<version>)."
    for pkg in libc6-dev linux-libc-dev libstdc++-12-dev libgcc-12-dev libvulkan1; do
        f=$(readlink -f "/usr/share/doc/$pkg")/changelog.Debian.gz
        top=$(zcat "$f" | head -1) || true   # (head closes the pipe early: zcat's SIGPIPE under pipefail is no error)
        [ -n "$top" ] || { echo "$pkg: no source version ($f): SOURCES.txt would have a gap" >&2; exit 1; }
        echo "$top" | sed -E "s/^([^ ]+) \(([^)]+)\).*/$pkg: source \1 version \2/"
    done
} > "$OUT/licenses/SOURCES.txt"
cp "$DEPS/SDL3-3.2.30/LICENSE.txt" "$OUT/licenses/SDL3.txt"
cp "$DEPS/opus-1.6.1/COPYING" "$OUT/licenses/opus.txt"
cp "$DEPS/libvpx-1.16.0/LICENSE" "$OUT/licenses/libvpx.txt"
cp "$DEPS/libvpx-1.16.0/PATENTS" "$OUT/licenses/libvpx-PATENTS.txt"
# (the Vulkan SDK's shaderc / glslang / SPIRV-Tools notices: the SDK release's LICENSE.txt files are not in its Linux
# tarball's x86_64 part; THIRD_PARTY_NOTICES.md L49 names them, as for Windows)
echo "glibc $(ldd --version | head -1 | awk '{print $NF}'), libstdc++ $(basename $GCC), made $(date -u +%F)" > "$OUT/VERSION"
tar -C "$(dirname "$OUT")" -cf - "$(basename "$OUT")" | zstd -19 -T0 -q -o "$OUT.tar.zst" -f
sha256sum "$OUT.tar.zst" | tee "$OUT.tar.zst.sha256"
du -sh "$OUT" "$OUT.tar.zst"
