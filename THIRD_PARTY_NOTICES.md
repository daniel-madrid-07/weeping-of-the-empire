# Third-party code

Code from other projects keeps its licence. The licence texts are in the files named in the last column.

| Our path | Origin | Licence | Notice |
|---|---|---|---|
| `engine/` (runtime, NVN-on-Vulkan layer, Maxwell shader translator, AArch64 recompiler and tools; imported as a git subtree, our changes on top) | [Muhammad1259-sigma/totk-recomp](https://github.com/Muhammad1259-sigma/totk-recomp) `a32f709` | MIT, "Copyright (c) 2026 the totk-recomp contributors" | `engine/LICENSE` |
| `engine/runtime/nvn/` (the NVN-on-Vulkan layer) | [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp), via totk-recomp | MIT, "Copyright (c) 2026 Peiikai" | `engine/runtime/nvn/LICENSE.LASRecomp` |
| `engine/third_party/imgui/` (unmodified) and the two SPIR-V shader arrays in `engine/runtime/nvn/vulkan/pc_overlay_vk.c` (from `backends/imgui_impl_vulkan.cpp`) | [ocornut/imgui](https://github.com/ocornut/imgui) v1.93.0 | MIT, "Copyright (c) 2014-2026 Omar Cornut"; `imstb_*.h`: MIT or public domain (in the files) | `engine/third_party/imgui/LICENSE.txt` |
| `engine/third_party/smaa/`, `engine/runtime/nvn/smaa/` | [iryoku/smaa](https://github.com/iryoku/smaa) `71c806a8` | MIT, Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez (2013) | `engine/third_party/smaa/LICENSE.txt`, `engine/runtime/nvn/smaa/LICENSE.txt` |
| `engine/third_party/opus/` (headers; `lib/opus.lib` is libopus 1.6.1, the vcpkg `x64-windows-static-md` release build with its debug information removed) | [Xiph.Org libopus](https://opus-codec.org) | BSD-3-Clause, plus the IETF royalty-free patent grants | `engine/third_party/opus/COPYING` |
| `engine/third_party/zstd/` (the decoder only, unmodified), compiled as one translation unit by `engine/runtime/rt_zstd.cpp` | [facebook/zstd](https://github.com/facebook/zstd) v1.5.7 | BSD-3-Clause, "Copyright (c) Meta Platforms, Inc. and affiliates" (zstd is dual BSD / GPL-2.0, used here under BSD) | `engine/third_party/zstd/LICENSE` |
| `engine/runtime/linux/stb_truetype.h` | [nothings/stb](https://github.com/nothings/stb) | MIT, "Copyright (c) 2017 Sean Barrett", or public domain | the end of the file |
| `engine/runtime/nvn/maxwell/soft_f64.h` (software binary64 for GPUs without shaderFloat64: the algorithms reimplemented in GLSL) | Berkeley SoftFloat Release 3e (John R. Hauser) | BSD-3-Clause, "Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018 The Regents of the University of California" | `licenses/SoftFloat-3e.txt` |
| The FSR 1 EASU / RCAS passes in `engine/runtime/nvn/vulkan/exec.c` (GLSL port) | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | MIT, "Copyright (c) 2021 Advanced Micro Devices, Inc." | `licenses/FidelityFX-FSR.txt` |
| `pipeline/extract.py`, `pipeline/nx/container.py`, `romfs.py`, `fonts.py`, `nso.py` (NSP / NCA / PFS0 / IVFC / RomFS / shared fonts / NSO to ELF); `tools/xlate/prepare_seed.py` (shader discovery); `games/totk/patches/sites_v100.py`, `arm64.py`, `games/totk/runtime/totk_patches.c`, `games/totk/overlays/*.py`, `games/totk/tests/verify_*.py` (1.0.0 patch sites and veneers, frame step, output size, heap formula, RomFS overlays, UI layouts, verifiers); `engine/tools/make_gameover_fix.py`, `make_quickmenu_fix.py` | [ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames) (`games/totk/scripts`, `games/totk/runtime`) | MIT, "Copyright (c) 2026 MacNativeGames contributors" | `licenses/MacNativeGames.txt` |
| The alpha-to-coverage dither in `engine/runtime/nvn/maxwell/translate.c` (the 2x2 coverage mask) | [Ryujinx](https://github.com/Ryujinx/Ryujinx) `EmitterContext.GenerateAlphaToCoverageDitherDiscard` | MIT, "Copyright (c) Ryujinx Team and Contributors" | `licenses/Ryujinx.txt` |
| `games/totk/overlays/ultrawide_layout.py`, `tall_layout.py`: which UI panes move and by how much for 21:9 and 16:10 (measurements of the game's layouts; the code is ours and MacNativeGames') | [TOTK-Ultrawide](https://github.com/Fruithapje21/TOTK-Ultrawide) (Fruithapje21) | GPL-3.0 project; facts only, no code or files | credit only |
| `games/totk/runtime/totk_patches.c` `frame_step_next` (EMA + owed-time repayment + 3 % per-frame limit; reimplemented, not copied) | [Revivedx/SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp) `575ad88e` | MIT, "Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors" | credit only |
| `engine/runtime/nvn/vulkan/shim_win.c` `keep_awake` (the display and system kept awake while the window is up), `engine/launcher/win/launcher.c` `mods_env` (an emulator-layout `mods` folder passed to the game), `tools/builder/builder.py` `jobs_for` (parallel compiles sized by RAM); ideas reimplemented, not copied | [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) | MIT, "Copyright (c) 2026 Valiantsin Dzerakh" | credit only |
| `games/totk/glyphs/kenney/` (the key and button glyphs' input: Keyboard & Mouse, Xbox Series and PlayStation Series `Vector/*.svg`) | Kenney, [Input Prompts](https://kenney.nl/assets/input-prompts) 1.5A | CC0 1.0 | `games/totk/glyphs/kenney/License.txt` |

## What the Setup exe carries or downloads

| What | Where it comes from | Licence |
|---|---|---|
| The part of LLVM the build runs (clang, lld-link, llvm-lib, llvm-profdata, llvm-rc, clang's headers and runtime libraries), inside the Setup exe | LLVM 23.1.1 (llvm/llvm-project releases) | Apache-2.0 WITH LLVM-exception (`tools/builder/gui/licenses/LLVM-LICENSE.txt`, shipped with it) |
| Vulkan headers, `vulkan-1.lib`, `shaderc_combined.lib` and the shaderc / SPIRV-Tools headers, inside the Setup exe (`tools/builder/gui/make_setup.py`; their `LICENSE.txt` and `NOTICE.txt` go with them) | LunarG Vulkan SDK 1.4.357.0 (Khronos Vulkan-Headers, Vulkan-Loader, google/shaderc, glslang, SPIRV-Tools) | Apache-2.0 OR MIT (headers), Apache-2.0, BSD-3-Clause, MIT |
| Python 3.13 (NuGet package), MinGit, the Python packages: downloaded by Setup on the player's PC from their publishers, checksums pinned (`tools/builder/gui/Builder.cs`, `tools/builder/toolchain.py`) | python.org / nuget.org, git-for-windows, PyPI | PSF-2.0, GPL-2.0 (MinGit, run as a separate program), each package's own |
| Microsoft's C++ runtime headers and libraries and the Windows SDK, only without an installed Visual Studio with C++: downloaded by `tools/builder/toolchain.py` from Microsoft's servers after the player accepts the licence; never redistributed by us | Microsoft (the Visual Studio 2022 Build Tools packages) | Microsoft Visual Studio Build Tools licence |

## Not in this repository

Fetched or found at build and run time, each under its own licence:

- **NVIDIA DLSS SDK** v310.9.1 (the NGX headers, `nvsdk_ngx_d.lib`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`): downloaded
  from [github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) by `tools/engine/build.py`, every file checked against a
  pinned SHA-256, under the NVIDIA RTX SDKs licence (its `LICENSE.txt` is downloaded with it). NVIDIA, DLSS and RTX are
  trademarks of NVIDIA Corporation.
- The Vulkan SDK (LunarG), LLVM / clang, the Microsoft C++ build tools and Windows SDK, Git for Windows (its
  libzstd.dll), Python, the Python packages you install for the Builder (zstandard, pillow, fonttools, numpy,
  cryptography, lz4, skia-pathops) and the ones the build installs in its own environment (capstone, lz4, ninja).

## Behaviour references (no code copied)

Atmosphère's cheat format (`engine/runtime/hle_cheat_vm.inc` is our own interpreter of it); yuzu, Eden and Ryujinx
for the behaviour of some Switch services, HID rumble and Maxwell instruction encodings (facts and formulas checked
against them, no code taken).
