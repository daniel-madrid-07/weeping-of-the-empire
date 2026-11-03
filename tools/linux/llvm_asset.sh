#!/bin/bash
# The compiler a player's Linux Setup downloads (linux-port, 2026-11-02): LLVM 23.1.1's official Linux build (the
# Windows Setup's version), only the programs the build runs and clang's own headers / runtime libraries (env.sh fetch
# keeps the same set), made runnable on any distribution: the official lld links Ubuntu 22.04's ICU 70 and liblzma, so
# those go into lib/ with libz, and every program finds them through RPATH $ORIGIN/../lib (patchelf).
# usage (in the root): bash /src/tools/linux/llvm_asset.sh /opt/llvm /out/llvm-asset  ->  /out/llvm-asset.tar.zst (+ .sha256)
# Licences: LLVM Apache-2.0 WITH LLVM-exception; ICU Unicode-3.0 (ICU license); zlib Zlib; xz-utils' liblzma 0BSD /
# public domain. Their notices go in licenses/.
set -euo pipefail
LLVM=${1:?llvm dir}; OUT=${2:?out dir}
[ -n "${NXR_IN_ROOT:-}" ] || { echo "run inside tools/linux/env.sh's root" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/licenses" "$OUT/bin"
# what a player's build runs: the compiler and linker, llvm-profdata (the PGO hot list), llvm-symbolizer (crash reports'
# host frames); the dev tools env.sh keeps (objdump, nm, readobj, dwarfdump, ar) stay out (~220 MB)
for b in clang clang++ clang-23 lld ld.lld llvm-profdata llvm-symbolizer; do cp -a "$LLVM/bin/$b" "$OUT/bin/"; done
# clang's headers and, of its runtime libraries, the builtins, the profile runtime (--pgo gen) and the crt objects; no
# sanitizers (~80 MB)
mkdir -p "$OUT/lib/clang" && cp -a "$LLVM/lib/clang/." "$OUT/lib/clang/"
find "$OUT/lib/clang" -path '*/lib/x86_64-unknown-linux-gnu/*' ! -name 'libclang_rt.builtins.a' \
     ! -name 'libclang_rt.profile.a' ! -name 'clang_rt.crt*.o' -delete
# the shared libraries the programs need beyond glibc (ldd's answer in the 22.04 root, which is what they were built for)
for b in "$OUT"/bin/*; do
    [ -L "$b" ] && continue
    ldd "$b" | awk '/=> \// {print $3}'
done | sort -u | while read -r lib; do
    case "$(basename "$lib")" in
        libc.so*|libm.so*|libpthread*|libdl.so*|librt.so*|libgcc_s*|libstdc++*|ld-linux*) ;;   # (glibc and GCC's: every system's)
        *) cp -L "$lib" "$OUT/lib/" ;;
    esac
done
for b in "$OUT"/bin/*; do
    [ -L "$b" ] || patchelf --set-rpath '$ORIGIN/../lib' "$b"
done
for lib in "$OUT"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
curl -fsSLo "$OUT/licenses/LLVM.txt" https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/LICENSE.TXT
echo "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee  $OUT/licenses/LLVM.txt" | sha256sum -c --quiet
for pkg in libicu70 zlib1g liblzma5; do cp "/usr/share/doc/$pkg/copyright" "$OUT/licenses/$pkg.copyright" 2>/dev/null || true; done
"$OUT/bin/clang" --version | head -1 > "$OUT/VERSION"
ls "$OUT/lib"
tar -C "$(dirname "$OUT")" -cf - "$(basename "$OUT")" | zstd -19 -T0 -q -o "$OUT.tar.zst" -f
sha256sum "$OUT.tar.zst" | tee "$OUT.tar.zst.sha256"
du -sh "$OUT" "$OUT.tar.zst"
