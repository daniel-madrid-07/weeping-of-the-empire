# Weeping of the Empire: Tears of the Kingdom, native on PC

<p align="center">
  <a href="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/daniel-madrid-07/weeping-of-the-empire?label=release"></a>
  <img alt="Windows 10/11 x64" src="https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-2a6fdb">
  <img alt="Vulkan 1.3" src="https://img.shields.io/badge/Vulkan-1.3-7fe3d4">
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-5f8a97"></a>
</p>

**Turns your own copy of *The Legend of Zelda: Tears of the Kingdom* (TotK, Switch, version 1.0.0) into a native Windows game.** The game's ARM64 code is recompiled ahead of time into x86-64, a host runtime answers the Switch's system services, and the graphics run on Vulkan. It is not an emulator: nothing is interpreted while you play.

**No game files are included, and none are ever shared.** For use with a game you own: you build it on your PC from a dump of your own console's game and your own keys. This project gives no help getting them. Not affiliated with or endorsed by Nintendo. [Read LEGAL.md](LEGAL.md).

Most of the code was written by AI coding agents directed by one developer, who sets priorities, plays test builds and decides what ships. It builds on MIT projects, [credited below](#credits): [how it was made](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/2).

<p align="center">
  <img src="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/download/v0.1.1/screenshot-gliding.jpg" alt="Gliding above the clouds" width="49%">
  <img src="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/download/v0.1.1/screenshot-fight.jpg" alt="A fight at a monster camp" width="49%"><br>
  <sub>Captured from the developer's own copy on a Ryzen 7 9800X3D + RTX 5080 (developer build): a 2560x1415 window, unlocked frame rate, the port's fps overlay at the top right.</sub>
</p>

- **Frame rate:** 30, 60, 90, 120 or unlocked, up to your monitor's refresh rate (240 fps at most), at the game's real speed.
- **Measured on one PC only** (Ryzen 7 9800X3D + RTX 5080, developer build, 2560x1440 window, 2133x1200 render, one run per place): 116.2-119.7 fps average at the 120 cap, 159.1-178.4 unlocked. Builds made by Setup get a list of the developer build's shaders instead of its full shader cache (the difference is not measured), so expect the same or lower. [Full table, and what is not measured](#performance-measured).
- **Display:** any resolution, ultrawide, borderless or exclusive fullscreen; VRR and HDR options (not measured); NVIDIA DLSS super resolution on RTX cards and DLSS frame generation on RTX 40 / 50 series (both optional), FSR 1, SMAA.
- **Controls:** keyboard and mouse with PC prompts in the game; Xbox, PlayStation and Switch controllers; settings window in game (F10).
- **Limits today:** game version 1.0.0 only, and a save from a later version may not load. Windows only. Dedicated AMD and Intel cards are not measured. First visits can stutter while shaders are built.

To hear about new releases, use Watch > Custom > Releases. Later game versions are being worked on; there is no date.

**Community:** ask questions in [Discussions > Q&A](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/categories/q-a),
share your play in [Show and tell](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/categories/show-and-tell),
and send how it runs on your hardware, with the log, as a
[performance report](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=performance.yml).

## Install in 3 steps

1. **Download** the Setup exe from the [latest release](https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/latest) and run it (no administrator rights needed). Setup is not code-signed, so Windows SmartScreen may warn the first time. Its SHA-256 is in the release notes.
2. **Drop in your files:** your dump of the game, version 1.0.0 (`.nsp` or `.xci`), and your `prod.keys`. Pick a folder on an NTFS drive with about 75 GB free and press **Install**.
3. **Wait once, then play.** Setup downloads the tools it needs, an optimisation profile (made by running the game on the developer's PC: function names and how often their branches ran) and a list of the shaders the game used there (hashes and render states; see [LEGAL.md](LEGAL.md) for both), decrypts and recompiles the game on your PC, and puts a shortcut on your desktop. Setup's own estimate is 30 to 60 minutes. One full install of an earlier version took about 33 minutes on a 16-thread CPU with 62 GB of RAM while other work was running, and its folder peaked at about 39 GB (Setup asks for 75 GB to be safe). Internet is needed during the install. If it stops, press Install again and it continues where it stopped.

Optional: your console's firmware, for its own Korean and Chinese fonts. Without it, Korean and Chinese text uses a
Windows font for that language, or English on a Windows that has none.
Building from a git clone instead: see [Build from a clone](#build-from-a-clone).

## Performance (measured)

Measured on the developer's PC: **AMD Ryzen 7 9800X3D (8 cores, 16 threads), NVIDIA RTX 5080**, Windows, a
2560x1440 window. Each row is one run at one place, averaged over the run; "1 % low" is the 1st percentile of frame
rates. A developer build from October 2026, the developer's everyday settings; DLSS and frame generation were
off in these runs.

| Place | Render resolution | 120 fps cap: average / 1 % low | Unlocked: average / 1 % low |
|---|---|---:|---:|
| Castle area, rain | 2133x1200 | 118.9 / 108 | 178.4 / 110 |
| Kakariko | 2133x1200 | 119.5 / 106 | 159.1 / 102 |
| The Depths | 2133x1200 | 119.7 / 107 | 198.6 / 123 (at 2096x1179) |
| Field camp fight | 2133x1200 | 116.2 / 84 | 159.5 / 101 |
| Castle area, rain | 3840x2160 | 119.2 / 107 | 151.0 / 94 |
| Field camp fight | 3840x2160 | 114.1 / 83 (a second run: 100.1 / 67) | 97.4 / 64 |

- **Video memory:** peak 5.07 GB at 2560x1440 and 6.17 GB at 3840x2160 (castle rain, unlocked). This was measured
  with another program holding 7.9 GB of the 16 GB card to stand in for an 8 GB card; the frame rate there was
  114.6 fps, 1 % low 37 (2560x1440) and 96.8 fps, 1 % low 26 (3840x2160); the lows come from a partly warm shader
  cache and, at 3840x2160, GPU-bound stretches.
- **Loading** (one run each): 1.63 s from launch to the first frame (1920x1080); 1.91 s from *Continue* to the first
  world frame in a newer build (30 fps cap; an older build with file tracing on took 3.42 s).
- **Stutter:** the first visit to a place or a fight can hitch while its shaders are built (from an empty cache: 17
  hitches, median 96 ms, worst 997 ms). Built shaders stay in the cache on disk for later visits.

**Setup builds:** one run so far, of v0.1.5 on the same PC: median 238.9 fps over a 300 s world run, unlocked with a
240 fps ceiling, a 1920x1080 window, 1600x900 render; no 1 % low was recorded. Fights and towns mostly stay lower.
Setup builds use the same optimisation profile, and a list of the developer build's shaders instead of its full
shader cache.

**Not measured yet:** dedicated AMD and Intel graphics cards
(AMD so far: one integrated GPU, see the FAQ); any CPU other than the one above; a real 8 GB card; laptops. Your
numbers are welcome: use the
[performance report](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=performance.yml).

## Features

| | |
|---|---|
| **Frame rate** | 30, 60, 90, 120 or unlocked (up to your monitor's refresh rate, 240 fps at most), with the game running at its real speed |
| **Display** | Any window size and render resolution, ultrawide, borderless or exclusive fullscreen, VRR, HDR |
| **Upscaling** | NVIDIA DLSS super resolution on RTX cards and DLSS frame generation on RTX 40 / 50 series (both optional), FSR 1, SMAA |
| **Controls** | Keyboard and mouse made for a PC, with keyboard and mouse glyphs in the game's prompts; Xbox, PlayStation and Switch controllers |
| **Settings** | In-game settings window (F10): graphics, controls, audio and the game's options |
| **Saves** | Import from an emulator or a console backup; automatic backups. A save from a later game version may not load |
| **Updates** | Run a newer release's Setup on the same folder: it rebuilds only what changed and keeps saves and settings |

Speed-ups are meant to leave the image exactly as it was; features that change the image (DLSS, FSR, frame
generation) are options.

## Status and known limits

- Only version **1.0.0** of the game is supported.
- **Windows 10 / 11** only. Linux and Steam Deck are not supported or tested.
- First visits can stutter while shaders are built (above).
- One graphics device loss was seen during a fight on the developer's PC. It has not been reproduced in several
  long test runs and its cause is unknown. If the game closes with a device-loss error, please send the newest
  `logs\totk-*.log` in a [bug report](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=bug_report.yml).
- Some visual differences from the Switch remain, among them lighting in parts of the Depths. Known problems are in
  [Issues](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues).
- PC settings live in the F10 window. Showing them in the game's own Options screen too is experimental: add
  `env.TOTK_OPTIONS_MENU=1` to the game folder's `player.txt`.
- Setup is not code-signed, so Windows SmartScreen may warn the first time.

## Requirements

- **Your own files:** the game, version 1.0.0 (`.nsp` or `.xci`, title `0100F2C0115B6000`) and your `prod.keys`;
  optionally your console's firmware, for its Korean and Chinese fonts.
- **CPU** with AVX2 (Intel Haswell / AMD Zen or newer) for the build we have tested. Processors with SSE4.2 but
  without AVX2 (Intel Sandy / Ivy Bridge, AMD FX, many Pentium / Celeron) get a compatible build that the Builder picks
  by itself; it is untested on such a processor: we have only seen it start on our own AVX2 PC, and its speed is
  unknown. Reports welcome. Setup refuses processors without SSE4.2. **GPU** with a current Vulkan 1.3 driver.
- **8 GB of RAM** (16 GB recommended: the build runs fewer compiles at once on less).
- **About 75 GB free** on an NTFS drive, and an internet connection during the install.

## FAQ

**Is this an emulator?** No. The game's code is translated once, on your PC, into a Windows program. Nothing is
interpreted while you play.

**Can I download a ready-made build?** No. Builds contain the game's recompiled code and data, so they are never
shared, here or anywhere else. Do not share yours.

**Where do I get the game and the keys?** From your own console, with your own tools. This project does not help with
that: issues or messages asking for game files, keys, firmware or builds are closed without reply.

**Does it work on AMD or Intel graphics?** Fixes for AMD's driver are in. On AMD we have run it only on the small
integrated Radeon (RDNA2) of the Ryzen 7 9800X3D: it reaches the world and plays without a device loss, at 13.3 fps
(1600x900 scene) or 31.2 fps (800x450), far below any gaming card. Dedicated AMD and Intel cards are not measured yet.
Please report how it runs.

**Why only version 1.0.0?** The patches and the recompiled code are tied to that version's code. Later versions are
being worked on; there is no date.

**Is my keys file safe?** Setup passes it to the build as a path. It is never printed, copied or uploaded, and the
whole build runs on your PC.

**Where are my saves?** `%APPDATA%\nxrecomp\TotK-engine`. Setup keeps them across updates.

## Build from a clone

`py tools/builder/gui/make_setup.py --builder` makes `Builder.exe` in the repository root; it builds the clone with
the tools installed on your PC (git, LLVM, Visual Studio Build Tools with C++, the Vulkan SDK, Python 3.11+; it lists
what is missing) into `build-work/release/`, next to the repository. Or run
`py tools/builder/builder.py <game> <keys> [<firmware>]`: `--help` lists the steps and options, `--dry-run` checks
everything without building, and `--install DIR` uses Setup's layout and fetches the tools itself. The clone must be a git clone (the build exports its sources with git); after a
`git pull` a build redoes only what changed.

### How it works

| Step | Code |
|---|---|
| Extract the game's code and data from your dump and verify every hash | `pipeline/` |
| Decompress the code modules, find the functions | `engine/tools/nso.py`, `engine/tools/funcs.py` |
| Apply the patches (frame rate, real-time speed, resolution, PC controls) | `games/totk/patches/` |
| Translate the AArch64 code to C++, compile it with clang, link it with the runtime | `engine/tools/recomp/`, `tools/engine/build.py` |
| Runtime: Switch system services, NVN on Vulkan, Maxwell shaders to SPIR-V, input, audio, PC menus | `engine/runtime/` |
| Data overlays made from your own files: other aspect ratios, PC glyphs, fixes above 30 fps | `games/totk/overlays/`, `games/totk/glyphs/`, `engine/tools/` |
| The playable folder and its launcher | `engine/launcher/win/` |

## Contributing

Bug and performance reports, and pull requests, are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

Built on the work of others, all under the MIT licence: the engine started from
[totk-recomp](https://github.com/Muhammad1259-sigma/totk-recomp), whose NVN layer comes from
[LAS Recomp](https://github.com/peiikai/LASRecomp); the extraction tools, the game patches and the layouts started
from [MacNativeGames](https://github.com/ly05010419/MacNativeGames); the frame pacing follows
[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp); a few small ideas from
[totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) are reimplemented. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
lists every third-party component and its licence. The logo and banner are original to this project.

## Legal

For use with a game you own, dumped from your own console. This repository contains no game code, data, assets, keys
or firmware, and gives no help obtaining them; requests for files are closed. Not affiliated with or endorsed by
Nintendo. *The Legend of Zelda* and Nintendo Switch are trademarks of Nintendo. Details: [LEGAL.md](LEGAL.md).
Our code: [MIT](LICENSE); third-party code keeps its own licence.
