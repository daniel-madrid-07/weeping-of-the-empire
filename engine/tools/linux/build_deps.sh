#!/usr/bin/env bash
# Build the third-party pieces of the Linux build from their official releases (checksum-pinned), the same versions as
# the Windows build where it has them:
#   SDL3 3.2.30      libsdl-org/SDL   shared (X11 + Wayland + PipeWire / PulseAudio / ALSA + udev / HIDAPI / D-Bus + Vulkan),
#                                     bundled as lib/libSDL3.so.0
#   Opus 1.6.1       xiph             static (hle_codec.cpp: the game's hardware Opus decoder; Windows: engine/third_party/opus 1.6.1)
#   libvpx 1.16.0    webmproject      static, the VP9 decoder only (movie_core.h VpxVp9Dec; Windows: engine/third_party/libvpx)
#   Vulkan SDK 1.4.357.0 (LunarG)     headers + libshaderc_combined.a (Windows: setup-vulkan-sdk.zip, the same SDK release)
#   hactool 1.4.0    SciresM/hactool  only with --hactool (build_from_xci.sh: NCA / romfs extraction)
# usage: build_deps.sh <deps dir> [--hactool] [--no-libs] [--jobs N]   (--no-libs: skip the libraries)
# Built in tools/linux/env.sh's Ubuntu 22.04 root (glibc 2.35) with the pinned LLVM, so the results run on any glibc >= 2.35.
# No -march: SDL3, Opus and libvpx pick their SIMD paths at run time, and the game runs on x86-64-v2 CPUs too.
set -euo pipefail
DEPS="${1:?usage: build_deps.sh <deps dir> [--hactool] [--jobs N]}"; shift
WANT_HACTOOL=0; WANT_LIBS=1; JOBS="$(nproc)"
while [ $# -gt 0 ]; do
  case "$1" in --hactool) WANT_HACTOOL=1 ;; --no-libs) WANT_LIBS=0 ;; --jobs) JOBS="$2"; shift ;; *) echo "unknown option $1" >&2; exit 2 ;; esac
  shift
done
CC="${CC:-clang}"; CXX="${CXX:-clang++}"
export AR="${AR:-llvm-ar}" RANLIB="${RANLIB:-llvm-ranlib}"
export TAR_OPTIONS=--no-same-owner   # (root in env.sh's user namespace: the archives' owners do not exist there)
mkdir -p "$DEPS"; DEPS="$(cd "$DEPS" && pwd)"

fetch() {   # url sha256 file
  if [ ! -f "$DEPS/$3" ]; then curl -fL --retry 3 -o "$DEPS/$3.part" "$1" && mv "$DEPS/$3.part" "$DEPS/$3"; fi
  echo "$2  $DEPS/$3" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $3" >&2; exit 1; }
}

if [ "$WANT_LIBS" = 1 ] && [ ! -f "$DEPS/sdl3-install/lib/libSDL3.so.0" ]; then
  echo "== SDL3 3.2.30"
  fetch https://github.com/libsdl-org/SDL/releases/download/release-3.2.30/SDL3-3.2.30.tar.gz \
        4c3b09330d866dc52eb65b66259a6684ad387252ca8c57901b3a2b534eb42e3d SDL3-3.2.30.tar.gz
  rm -rf "$DEPS/SDL3-3.2.30" "$DEPS/sdl3-build"; tar -C "$DEPS" -xf "$DEPS/SDL3-3.2.30.tar.gz"
  CC="$CC" CXX="$CXX" cmake -G Ninja -S "$DEPS/SDL3-3.2.30" -B "$DEPS/sdl3-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS/sdl3-install" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
    -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON \
    -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=OFF -DSDL_LIBUDEV=ON -DSDL_DBUS=ON -DSDL_VULKAN=ON \
    -DSDL_KMSDRM=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_UNIX_CONSOLE_BUILD=OFF
  ninja -C "$DEPS/sdl3-build" -j"$JOBS" install
fi

if [ "$WANT_LIBS" = 1 ] && [ ! -f "$DEPS/opus-install/lib/libopus.a" ]; then
  echo "== Opus 1.6.1"
  fetch https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz \
        6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus-1.6.1.tar.gz
  rm -rf "$DEPS/opus-1.6.1"; tar -C "$DEPS" -xf "$DEPS/opus-1.6.1.tar.gz"
  CC="$CC" CFLAGS="-O2 -fPIC" cmake -G Ninja -S "$DEPS/opus-1.6.1" -B "$DEPS/opus-1.6.1/build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF -DCMAKE_INSTALL_PREFIX="$DEPS/opus-install" \
    -DCMAKE_INSTALL_LIBDIR=lib
  ninja -C "$DEPS/opus-1.6.1/build" -j"$JOBS" install
fi

if [ "$WANT_LIBS" = 1 ] && [ ! -f "$DEPS/vpx-install/lib/libvpx.a" ]; then
  echo "== libvpx 1.16.0 (VP9 decoder)"
  fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.16.0.tar.gz \
        7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c libvpx-1.16.0.tar.gz
  rm -rf "$DEPS/libvpx-1.16.0"; tar -C "$DEPS" -xf "$DEPS/libvpx-1.16.0.tar.gz"
  ( cd "$DEPS/libvpx-1.16.0" && CC="$CC" CXX="$CXX" LD="$CC" ./configure --target=x86_64-linux-gcc --prefix="$DEPS/vpx-install" \
      --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --enable-static --disable-shared --enable-pic \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests --enable-runtime-cpu-detect \
    && make -j"$JOBS" && make install )
fi

if [ "$WANT_LIBS" = 1 ] && [ ! -f "$DEPS/vulkan-sdk/lib/libshaderc_combined.a" ]; then
  echo "== Vulkan SDK 1.4.357.0: headers + shaderc_combined"
  fetch https://sdk.lunarg.com/sdk/download/1.4.357.0/linux/vulkansdk-linux-x86_64-1.4.357.0.tar.xz \
        0f09bf6a0625e346bf004be70b92907e934a4c76606b323441b2baf3a5a0e66d vulkansdk-linux-x86_64-1.4.357.0.tar.xz
  rm -rf "$DEPS/vulkan-sdk"; mkdir -p "$DEPS/vulkan-sdk"
  tar -C "$DEPS/vulkan-sdk" --strip-components=2 -xJf "$DEPS/vulkansdk-linux-x86_64-1.4.357.0.tar.xz" \
    1.4.357.0/x86_64/include/vulkan 1.4.357.0/x86_64/include/vk_video 1.4.357.0/x86_64/include/shaderc \
    1.4.357.0/x86_64/lib/libshaderc_combined.a
fi
# (the Vulkan headers mkninja_linux.py takes: deps/vulkan-headers/include)
if [ "$WANT_LIBS" = 1 ] && [ ! -e "$DEPS/vulkan-headers/include" ]; then
  mkdir -p "$DEPS/vulkan-headers"; ln -s ../vulkan-sdk/include "$DEPS/vulkan-headers/include"
fi

if [ "$WANT_HACTOOL" = 1 ] && [ ! -x "$DEPS/hactool/hactool" ]; then
  echo "== hactool 1.4.0"
  fetch https://github.com/SciresM/hactool/archive/refs/tags/1.4.0.tar.gz \
        1373c98ef8c58da42cb1e81b361283955caad0e1b8b5ad389b0ee49b7a21da43 hactool-1.4.0.tar.gz
  rm -rf "$DEPS/hactool-src"; mkdir -p "$DEPS/hactool-src"; tar -C "$DEPS/hactool-src" --strip-components=1 -xf "$DEPS/hactool-1.4.0.tar.gz"
  ( cd "$DEPS/hactool-src" && cp config.mk.template config.mk && make -j"$JOBS" CC="${HACTOOL_CC:-$CC}" )
  mkdir -p "$DEPS/hactool"; cp "$DEPS/hactool-src/hactool" "$DEPS/hactool/hactool"
fi
echo "deps ready in $DEPS"
