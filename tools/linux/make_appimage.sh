#!/bin/bash
# Weeping of the Empire's Linux AppImage (linux-port, 2026-11-02): the Setup / Play window and the Python it runs on.
# Nothing of the game and no compiler is in it: Setup fetches this release's source (git) and the two pinned Linux
# assets (setup-linux-llvm, setup-linux-sdk) and builds the game from the player's own dump, as on Windows.
#   AppDir/AppRun                   -> usr/python/bin/python3 -I usr/app/setup_gui.py "$@"   (--play, --setup work too)
#   AppDir/usr/python               python-build-standalone 3.13 (CPython, PSF-2.0; Tk for the window), trimmed
#   AppDir/usr/app/setup_gui.py     the window; release.json: the tag and repository Setup fetches
#   AppDir/weeping-of-the-empire.{desktop,svg}   our own name and mark (no game artwork)
# Packed with appimagetool and the AppImage type-2 runtime (both MIT), pinned below.
# usage: tools/linux/make_appimage.sh <tag> [<owner/repo>] [<out dir>]      (in env.sh's root or on any Linux x86-64)
#   NXR_COMMIT=<40 hex>: the commit the tag names (release-audit's audited one), needed before the tag is public; Setup
#   builds only that commit (release.json "commit", as the Windows Setup's Release.Commit)
#   NXR_BUNDLE_DIR=<dir>: a test build for testers before a release: <dir>'s src.bundle (a git bundle with the tag) and
#   the two Linux archives go inside (AppDir/usr/bundle, ~150 MB more), so Setup needs no public tag or release asset
set -euo pipefail
TAG=${1:?release tag, e.g. v0.1.9}; REPO_GH=${2:-daniel-madrid-07/weeping-of-the-empire}; OUT=${3:-/out/appimage}
HERE=$(cd "$(dirname "$0")" && pwd)
DL=$OUT/dl; APP=$OUT/AppDir
PY_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.13.18+20261026-x86_64-unknown-linux-gnu-install_only.tar.gz
PY_SHA=126343c54daff4068efc1ac269518d0cc202d57857794948103dd1454e8881be
# (the same build's full archive: only for python/licenses/, the licences of the libraries the interpreter bundles,
#  OpenSSL, SQLite, libffi, Tcl/Tk, xz, bzip2, zlib, ncurses, ...; install_only has CPython's own LICENSE.txt alone)
PYFULL_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.13.18+20261026-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst
PYFULL_SHA=f003449a05408c2e4b184f2a7abd1ea277081ff1a9100289075243ab5ed81207
TOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage
TOOL_SHA=46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64   # (a tagged release, not continuous)
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

fetch() {   # url sha file (sha empty: printed, to pin)
    if [ ! -f "$DL/$3" ]; then curl -fsSL --retry 3 -o "$DL/$3.part" "$1"; mv "$DL/$3.part" "$DL/$3"; fi
    local got; got=$(sha256sum "$DL/$3" | cut -d' ' -f1)
    if [ -n "$2" ] && [ "$got" != "$2" ]; then echo "$3: sha256 $got, pinned $2 (removed: fetched again next run)" >&2; rm -f "$DL/$3"; exit 1; fi
    [ -n "$2" ] || echo "pin $3 sha256 $got"
}
mkdir -p "$DL"; rm -rf "$APP"; mkdir -p "$APP/usr/app"
fetch "$PY_URL" "$PY_SHA" python.tar.gz
fetch "$PYFULL_URL" "$PYFULL_SHA" python-full.tar.zst
fetch "$TOOL_URL" "$TOOL_SHA" appimagetool
fetch "$RUNTIME_URL" "$RUNTIME_SHA" runtime-x86_64-20251108
chmod +x "$DL/appimagetool"

# Python: what Setup and the window run (the build's own packages go into <install>/tools/python, a copy)
tar -xzf "$DL/python.tar.gz" -C "$APP/usr"   # (-> usr/python)
P=$APP/usr/python/lib/python3.13
rm -rf "$P/test" "$P/idlelib" "$P/turtledemo" "$P/tkinter/test" "$P/lib2to3" "$APP/usr/python/include" \
       "$APP/usr/python/share/man" "$P/config-3.13-x86_64-linux-gnu/libpython3.13.a"
find "$APP/usr/python" -name '__pycache__' -prune -exec rm -rf {} +

PEEL=$(mktemp -d); trap 'rm -rf "$PEEL"' EXIT   # (a scratch repository: the commit the AppImage is made for)
git -C "$PEEL" init -q
if [ -n "${NXR_BUNDLE_DIR:-}" ]; then
    # a test build (a version with no public tag or Linux assets yet): src.bundle (a git bundle holding refs/tags/$TAG)
    # and the two toolchain archives go inside; Setup fetches and unpacks those instead of downloading
    mkdir -p "$APP/usr/bundle"
    for f in src.bundle setup-linux-llvm.tar.zst setup-linux-sdk.tar.zst; do cp "$NXR_BUNDLE_DIR/$f" "$APP/usr/bundle/"; done
    git bundle list-heads "$APP/usr/bundle/src.bundle" | grep -q " refs/tags/$TAG\$" || { echo "src.bundle has no tag $TAG" >&2; exit 1; }
    # the tag's commit, peeled (list-heads gives an annotated tag's own object, which Setup's FETCH_HEAD^{commit} never
    # matches): the bundle's tag fetched into the scratch repository
    git -C "$PEEL" fetch -q "$APP/usr/bundle/src.bundle" "refs/tags/$TAG:refs/tags/$TAG"
    FOUND=$(git -C "$PEEL" rev-parse --verify -q "refs/tags/$TAG^{commit}") || { echo "src.bundle: $TAG names no commit" >&2; exit 1; }
    BUNDLE=', "bundle": true'
else
    # the public tag's commit (annotated: its peeled ^{} line), when it is already published
    FOUND=$(git ls-remote "https://github.com/$REPO_GH.git" "refs/tags/$TAG" "refs/tags/$TAG^{}" | sort -k2 | tail -1 | cut -f1) ||
        { echo "could not ask github.com/$REPO_GH for $TAG (network?)" >&2; exit 1; }
    BUNDLE=''
fi
# The commit Setup builds (as Builder.cs's Release.Commit): it refuses fetched source that is another. NXR_COMMIT
# (release-audit's audited public commit) before the tag is public; both known: they must agree.
COMMIT=$(echo "${NXR_COMMIT:-$FOUND}" | tr 'A-F' 'a-f')
[ -n "$COMMIT" ] || { echo "$TAG is not on github.com/$REPO_GH yet: give NXR_COMMIT=<the audited public commit>" >&2; exit 1; }
echo "$COMMIT" | grep -qE '^[0-9a-f]{40}$' || { echo "NXR_COMMIT $COMMIT: a full 40-digit commit id is needed" >&2; exit 1; }
if [ -n "${NXR_COMMIT:-}" ] && [ -n "$FOUND" ] && [ "$COMMIT" != "$FOUND" ]; then
    echo "NXR_COMMIT $COMMIT is not the commit $TAG names ($FOUND)" >&2; exit 1
fi
printf '{"tag": "%s", "repo": "%s", "commit": "%s"%s}\n' "$TAG" "$REPO_GH" "$COMMIT" "$BUNDLE" > "$APP/usr/app/release.json"
# The window's own files as that commit has them, not this tree's copy (which can differ from the published blob:
# release-audit 11-04, a comment in setup_gui.py). The published tag is fetched (one commit); before it is public
# (NXR_COMMIT alone) nothing can be fetched: this tree's files, said so.
[ -n "$BUNDLE" ] || [ -z "$FOUND" ] || git -C "$PEEL" fetch -q --depth 1 "https://github.com/$REPO_GH.git" "refs/tags/$TAG:refs/tags/$TAG"
if git -C "$PEEL" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then SRCREV=$COMMIT
else SRCREV=''; echo "$COMMIT is not public yet: the window's files are this tree's (check them against the export)" >&2; fi
appfile() {   # tools/linux/appimage/<name> -> stdout
    if [ -n "$SRCREV" ]; then git -C "$PEEL" cat-file blob "$SRCREV:tools/linux/appimage/$1"; else cat "$HERE/appimage/$1"; fi
}
appfile setup_gui.py > "$APP/usr/app/setup_gui.py"
appfile AppRun > "$APP/AppRun"; chmod 755 "$APP/AppRun"
for f in weeping-of-the-empire.desktop weeping-of-the-empire.svg; do appfile "$f" > "$APP/$f"; done
ln -sf weeping-of-the-empire.svg "$APP/.DirIcon"
mkdir -p "$APP/usr/share/licenses"
cp "$APP/usr/python/lib/python3.13/LICENSE.txt" "$APP/usr/share/licenses/python.txt"
tar --zstd -xf "$DL/python-full.tar.zst" -C "$APP/usr/share/licenses" python/licenses
mv "$APP/usr/share/licenses/python/licenses" "$APP/usr/share/licenses/python-bundled" && rmdir "$APP/usr/share/licenses/python"
# (the type-2 runtime is part of every AppImage made with it: its MIT licence goes inside)
fetch https://raw.githubusercontent.com/AppImage/type2-runtime/20251108/LICENSE \
      aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1 runtime-LICENSE
cp "$DL/runtime-LICENSE" "$APP/usr/share/licenses/appimage-type2-runtime.txt"

NAME=WeepingOfTheEmpire-$TAG-x86_64.AppImage
# Reproducible: the same tag gives the same bytes, so anyone can rebuild it and compare with SHA256SUMS. Every file in
# the AppDir gets one fixed time (the inodes' times); appimagetool sets the image's own time itself (mksquashfs then
# refuses SOURCE_DATE_EPOCH: "can't be used at the same time", so it is unset for the tool).
EPOCH=315532800   # (1980-01-01: no meaning, only fixed)
find "$APP" -exec touch -h -d "@$EPOCH" {} +
unset SOURCE_DATE_EPOCH
# (no FUSE in a build root: appimagetool runs extracted; the runtime from the pinned file, not fetched by the tool)
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DL/appimagetool" --no-appstream --runtime-file "$DL/runtime-x86_64-20251108" \
    "$APP" "$OUT/$NAME"
# (the bare file name: a player checks the download with 'sha256sum -c <name>.sha256' in their own folder)
(cd "$OUT" && sha256sum "$NAME") | tee "$OUT/$NAME.sha256"
du -h "$OUT/$NAME"
