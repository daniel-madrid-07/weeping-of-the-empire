# engine

The recompiler and the runtime the game runs on. This folder started as a copy of
[totk-recomp](https://github.com/Muhammad1259-sigma/totk-recomp) (MIT, `LICENSE`), a static recompilation of the
same game for Linux; its NVN layer comes from [LAS Recomp](https://github.com/peiikai/LASRecomp)
(MIT, `runtime/nvn/LICENSE.LASRecomp`). Our Windows port, renderer, PC controls and menus are built on top of it.

| Path | What |
|---|---|
| `tools/nso.py`, `tools/funcs.py` | decompress the NSO modules, find the functions |
| `tools/recomp/gen.py`, `gen_ext.py`, `abi_scan.py` | the AArch64 to C++ recompiler |
| `tools/recomp/mkninja_win.py` | the ninja build of the generated code and the runtime (driven by `tools/engine/build.py`) |
| `tools/rstb_slack.py`, `make_gameover_fix.py`, `make_quickmenu_fix.py` | data fixes made from your own RomFS |
| `runtime/` | the Switch system services (`hle_*.cpp`), the PC layer (`pc_*.cpp`: window, input, settings, F10 menu), the game hooks (`totk_*.cpp`) |
| `runtime/nvn/` | NVN on Vulkan (`vulkan/`) and the Maxwell shader to SPIR-V translator (`maxwell/`) |
| `launcher/win/` | the launcher, the save manager and the release folder (`package.py`) |
| `third_party/` | Dear ImGui, SMAA, libopus (see `THIRD_PARTY_NOTICES.md` at the root) |

The NVIDIA DLSS SDK is not in this repository: `tools/engine/build.py` downloads it (pinned, hash-checked) into the
build's source snapshot and the work folder.
