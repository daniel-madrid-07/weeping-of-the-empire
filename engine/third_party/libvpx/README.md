# libvpx (VP9 decoder only)

The engine's software VP9 decoder (`engine/runtime/movie_core.h` `VpxVp9Dec`), used on Windows when Media Foundation
has no VP9 decoder (a Windows without the Microsoft Store's "VP9 Video Extensions": LTSC, N, offline images; issues
#22, #25), and always on Linux (the system libvpx there).

- `include/vpx/`: the decoder API headers of libvpx 1.16.0, unmodified.
- `lib/vpx.lib`: the members of vcpkg's `libvpx:x64-windows-static-md@1.16.0#3` `vpx.lib` that the VP9 decoder links
  (147 of 251; no encoders, no VP8), debug sections stripped. Made by `subset.py <vcpkg installed\x64-windows-static-md>`.
- `COPYING`: libvpx's BSD-3-Clause license and the ISC license of the x86inc assembly macros built into it, as vcpkg
  ships them. libvpx also carries Google's patent grant: https://chromium.googlesource.com/webm/libvpx/+/refs/tags/v1.16.0/PATENTS
