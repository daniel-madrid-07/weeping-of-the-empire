#!/bin/bash
# The Linux build environment: an Ubuntu 22.04 root (glibc 2.35, the Linux build's baseline) entered rootless with
# unshare, plus the pinned LLVM (the Windows build's version). Dev builds and the release assets (deps, sysroot,
# AppImage) are made in it, so a dev build links against what players get. Nothing here needs root or touches the host
# system; everything lives under $NXR_LINUX (default ~/nxr-linux, keep it on the Linux filesystem, not /mnt/c).
#
# usage: tools/linux/env.sh fetch          download + verify the 22.04 base root and LLVM, unpack them
#        tools/linux/env.sh apt            add the build packages to the root (download + unpack, no maintainer scripts)
#        tools/linux/env.sh run CMD...     run CMD inside the root (repo at /src, $NXR_LINUX/out at /out, LLVM at /opt/llvm)
#        tools/linux/env.sh shell          an interactive shell inside the root
set -euo pipefail

R=${NXR_LINUX:-$HOME/nxr-linux}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)

LLVM_VER=23.1.1     # = tools/builder/toolchain.py LLVM['version']
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.xz
LLVM_SHA=832aeb58d105de1cabc7b982dd2c65de0610f7377df48ae8fc2dd8e97420a15c
LLVM_BIN="clang clang++ clang-23 clang-cpp lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-objdump llvm-objcopy llvm-strip
          llvm-readobj llvm-readelf llvm-profdata llvm-symbolizer llvm-cxxfilt llvm-dwarfdump"
BASE_URL=https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04.5-base-amd64.tar.gz
BASE_SHA=242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a

# build tools, headers SDL3 compiles its X11 / Wayland / PipeWire / PulseAudio / ALSA / KMSDRM backends against (SDL
# loads those libraries at run time), libc / libstdc++ for the sysroot, Mesa's lavapipe for a GPU-less smoke run
PKGS="curl desktop-file-utils cmake ninja-build pkg-config make nasm python3 python3-venv git ca-certificates file xz-utils zstd patchelf
      libc6-dev linux-libc-dev libstdc++-12-dev libgcc-12-dev
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev
      libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgles-dev libdrm-dev libgbm-dev
      libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libusb-1.0-0-dev libvulkan-dev
      mesa-vulkan-drivers"

fetch() {
    mkdir -p "$R/dl" "$R/root" "$R/llvm" "$R/out"
    cd "$R/dl"
    [ -f base.tar.gz ] || curl -fsSLo base.tar.gz "$BASE_URL"
    echo "$BASE_SHA  base.tar.gz" | sha256sum -c --quiet
    [ -f llvm.tar.xz ] || curl -fsSLo llvm.tar.xz "$LLVM_URL"
    local got; got=$(sha256sum llvm.tar.xz | cut -d' ' -f1)
    if [ -n "$LLVM_SHA" ] && [ "$got" != "$LLVM_SHA" ]; then echo "LLVM sha256 $got, pinned $LLVM_SHA" >&2; exit 1; fi
    echo "LLVM $LLVM_VER sha256 $got"
    [ -x "$R/root/bin/bash" ] || tar -xzf base.tar.gz -C "$R/root" --exclude='./dev/*'
    # what the build runs: the programs it calls and clang's own headers / runtime libraries (toolchain.py LLVM_KEEP; the
    # archive's bin/ alone is 9 GB: flang, mlir, lldb, bolt, ...)
    if [ ! -x "$R/llvm/bin/clang" ]; then
        local top=LLVM-$LLVM_VER-Linux-X64
        tar -xJf llvm.tar.xz -C "$R/llvm" --strip-components=1 --wildcards '*/lib/clang/*' \
            $(for b in $LLVM_BIN; do echo "$top/bin/$b"; done)
    fi
    cp /etc/resolv.conf "$R/root/etc/resolv.conf"
    "$R/llvm/bin/clang" --version | head -1
}

# enter the root: a user + mount + pid namespace where we are uid 0, /dev and /proc from the host, the repo, the out dir
# and LLVM bound in
run() {
    [ -x "$R/root/bin/bash" ] || { echo "no root at $R/root: run 'env.sh fetch' first" >&2; exit 1; }
    mkdir -p "$R/root/src" "$R/root/out" "$R/root/opt/llvm" "$R/out"
    unshare -r -m -p --fork --mount-proc="$R/root/proc" /bin/bash -c '
        R=$1 SRC=$2; shift 2
        mount --rbind /dev "$R/root/dev"
        mount --bind "$SRC" "$R/root/src"
        mount --bind "$R/out" "$R/root/out"
        mount --bind "$R/llvm" "$R/root/opt/llvm"
        # NXR_HOST_WORK: a host folder at /work-host, read-only (a dev build reads the Windows work folder'"'"'s extracted
        # game files from there; nothing in the root writes to it)
        if [ -n "${NXR_HOST_WORK:-}" ]; then
            mkdir -p "$R/root/work-host" && mount --bind "$NXR_HOST_WORK" "$R/root/work-host" &&
                mount -o remount,bind,ro "$R/root/work-host"
        fi
        [ -d /tmp/.X11-unix ] && mkdir -p "$R/root/tmp/.X11-unix" && mount --bind /tmp/.X11-unix "$R/root/tmp/.X11-unix"
        [ -d /mnt/wslg ] && mkdir -p "$R/root/mnt/wslg" && mount --rbind /mnt/wslg "$R/root/mnt/wslg"
        exec chroot "$R/root" /usr/bin/env -i HOME=/root TERM="${TERM:-dumb}" LANG=C.UTF-8 \
            PATH=/opt/llvm/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin NXR_IN_ROOT=1 \
            DISPLAY="${DISPLAY:-}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:+/mnt/wslg/runtime-dir}" \
            PULSE_SERVER="${PULSE_SERVER:-}" "$@"' _ "$R" "$SRC" "$@"
}

apt_add() {
    # apt resolves and downloads as root inside the namespace; the packages are unpacked with tar (no chown to other
    # users, no maintainer scripts: build headers and tools need none), then ldconfig indexes the libraries and the one
    # postinst that matters runs by hand (ca-certificates: the bundle curl and git read)
    run /bin/bash -c "
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get -o APT::Sandbox::User=root -qq update
        apt-get -o APT::Sandbox::User=root -qq -y --download-only install $(echo $PKGS)
        for d in /var/cache/apt/archives/*.deb; do dpkg-deb --fsys-tarfile \"\$d\" | tar -x --no-same-owner --keep-directory-symlink -C /; done
        ldconfig
        (cd /usr/share/ca-certificates && find . -name '*.crt' | sed 's#^\./##' | sort) > /etc/ca-certificates.conf
        update-ca-certificates --fresh >/dev/null
        apt-get clean
        cmake --version | head -1; python3 --version"
}

case "${1:-}" in
    fetch) fetch ;;
    apt) apt_add ;;
    run) shift; run "$@" ;;
    shell) run /bin/bash -l ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
