# Weeping of the Empire: Tears of the Kingdom, native on PC

<p align="center">
  <a href="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/daniel-madrid-07/weeping-of-the-empire?label=release"></a>
  <img alt="Windows 10/11 x64" src="https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-2a6fdb">
  <img alt="Vulkan 1.3" src="https://img.shields.io/badge/Vulkan-1.3-7fe3d4">
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-5f8a97"></a>
</p>

**Turns your own copy of *The Legend of Zelda: Tears of the Kingdom* (TotK, Switch, version 1.0.0, and 1.4.3 as a beta) into a native Windows game.** The game's ARM64 code is recompiled ahead of time into x86-64, a host runtime answers the Switch's system services, and the graphics run on Vulkan. It is not an emulator: nothing is interpreted while you play.

**No game files are included, and none are ever shared.** For use with a game you own: you build it on your PC from a dump of your own console's game and your own keys. This project gives no help getting them. Not affiliated with or endorsed by Nintendo. [Read LEGAL.md](LEGAL.md).

Most of the code was written by AI coding agents directed by one developer, who sets priorities, plays test builds and decides what ships. It builds on MIT projects, [credited below](#credits): [how it was made](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/2).

<p align="center">
  <img src="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/download/v0.1.1/screenshot-gliding.jpg" alt="Gliding above the clouds" width="49%">
  <img src="https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/download/v0.1.1/screenshot-fight.jpg" alt="A fight at a monster camp" width="49%"><br>
  <sub>Captured from the developer's own copy on a Ryzen 7 9800X3D + RTX 5080 (developer build): a 2560x1415 window, unlocked frame rate, the port's fps overlay at the top right.</sub>
</p>

- **Frame rate:** 30 to 240 (40 and 45 for a PC that cannot hold 60, and the usual monitor rates 60, 75, 90, 120, 144, 150, 165, 180, 200, 240) or unlocked, up to your monitor's refresh rate (240 fps at most), at the game's real speed.
- **Measured on one PC only** (Ryzen 7 9800X3D + RTX 5080, developer build, 2560x1440 window, 2133x1200 render, one run per place): 116.2-119.7 fps average at the 120 cap, 159.1-178.4 unlocked. Builds made by Setup get a list of the developer build's shaders instead of its full shader cache (the difference is not measured), so expect the same or lower. [Full table, and what is not measured](#performance-measured).
- **Display:** any resolution, ultrawide, borderless or exclusive fullscreen; VRR and HDR options (not measured); colour profiles (Vivid, Brighter dark tones, Custom); upscaling and frame generation as options: NVIDIA DLSS / DLAA on RTX cards with DLSS frame generation x2-x4 on RTX 40 / 50, AMD FSR 3.1 upscaling and frame generation on any GPU, FSR 1, SMAA. Setup downloads the DLSS files itself: nothing to copy by hand.
- **Controls:** keyboard and mouse with PC prompts in the game; Xbox, PlayStation and Switch controllers; settings window in game (F10). Every key and every setting: [CONTROLS.md](CONTROLS.md), [SETTINGS.md](SETTINGS.md).
- **Limits today:** game version 1.0.0, plus 1.4.3 as a beta (the same frame rates as 1.0.0, fewer PC features); a save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it (the original is kept). Windows only. Dedicated AMD and Intel cards are not measured. First visits can stutter while shaders are built.

To hear about new releases, use Watch > Custom > Releases. Later game versions are being worked on; there is no date.

**Community:** ask questions in [Discussions > Q&A](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/categories/q-a),
share your play in [Show and tell](https://github.com/daniel-madrid-07/weeping-of-the-empire/discussions/categories/show-and-tell),
and send how it runs on your hardware, with the log, as a
[performance report](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=performance.yml).
Players also meet on Reddit, in [r/WeepingOfTheEmpire](https://www.reddit.com/r/WeepingOfTheEmpire/).

## Install in 3 steps

1. **Download** the Setup exe from the [latest release](https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/latest) and run it (no administrator rights needed). Setup is not code-signed, so Windows SmartScreen may warn the first time. Its SHA-256 is in the release notes.
2. **Drop in your files:** your dump of the game, version 1.0.0 (`.nsp` or `.xci`; a compressed `.nsz` / `.xcz` works too, its game data is copied out once: 17 GB more; so does an unencrypted `.nup`, which needs no `prod.keys`), and your `prod.keys` (for the 1.4.3 beta, also your 1.4.3 update dump in the *Update (optional)* row, `.nsp`, `.xci`, `.nsz` or `.xcz`; a `.nup` that holds the update goes there as well, the same file, and then no `prod.keys` either). Pick a folder on an NTFS drive with about 15 GB free (Setup says if it needs more) and press **Install**.
3. **Wait once, then play.** Setup downloads the tools it needs, an optimisation profile (made by running the game on the developer's PC: function names and how often their branches ran) and a list of the shaders the game used there (hashes and render states; see [LEGAL.md](LEGAL.md) for both), decrypts and recompiles the game on your PC, and puts two shortcuts on your desktop: the game, and *Weeping of the Empire - Settings*, which opens the settings window before the game starts (holding Ctrl while you start the game does the same). Setup's own estimate is 30 to 60 minutes. One full install of an earlier version took about 33 minutes on a 16-thread CPU with 62 GB of RAM while other work was running, and its folder peaked at about 39 GB. Since v0.1.8 the game reads its data straight from your dump instead of a 17 GB copy, so keep the dump where it is: if you move it, the game asks you to pick it again. With the dump on a USB, removable or network drive, or with **Copy the game files** ticked in Setup, the data is copied instead (17 GB more) and the dump can then go. An install from an earlier version keeps its copy; untick **Copy the game files** to free those 17 GB (the dump then has to stay). The install then holds a key made from your keys (see [LEGAL.md](LEGAL.md)): do not share the install folder. Internet is needed during the install. If it stops, press Install again and it continues where it stopped.

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
| **Frame rate** | 30 to 240 (30, 40, 45, 60, 75, 90, 120, 144, 150, 165, 180, 200, 240) or unlocked (up to your monitor's refresh rate, 240 fps at most), with the game running at its real speed |
| **Display** | Any window size and render resolution, ultrawide 21:9 / 32:9, triple-screen 48:9, 16:10 and 3:2 (Surface, Framework) (F10 > Graphics > Game patches > Aspect ratio; from v0.1.9 it follows your window's shape by default), borderless or exclusive fullscreen, VRR, HDR |
| **Upscaling** | NVIDIA DLSS super resolution and DLAA on RTX cards, DLSS frame generation x2 / x3 / x4 on RTX 40 / 50 series, AMD FSR 3.1 upscaling and frame generation on any GPU, FSR 1, SMAA, all optional. Setup downloads NVIDIA's DLSS files itself (pinned by hash), so there is nothing to copy by hand |
| **Colour** | The game's own colours by default; Vivid, Brighter dark tones, or Custom brightness, contrast, saturation and gamma (F10 > Graphics > Colour) |
| **Controls** | Keyboard and mouse made for a PC, with keyboard and mouse glyphs in the game's prompts, gameplay and menu keys rebindable; Xbox, PlayStation and Switch controllers, back paddles of an Xbox Elite or DualSense Edge. Default keys: [CONTROLS.md](CONTROLS.md) |
| **Settings** | In-game settings window (F10): graphics, controls, audio and the game's options, most applying at once, with a help line for each row. Every row: [SETTINGS.md](SETTINGS.md) |
| **Mods** | RomFS mods made for emulators or the Switch, in a `mods` folder next to the game (see the FAQ); order and on / off in F10 > Mods; ExeFS code patches (.ips / .pchtxt) and Atmosphere cheats (F10 > Cheats) |
| **Saves** | Import from an emulator or a console backup; automatic backups. Saves of later game versions (1.1.x, 1.2.x, 1.4.x) are converted to 1.0.0 on import, the original kept in the backups |
| **Updates** | Run a newer release's Setup on the same folder: it rebuilds only what changed and keeps saves and settings |
| **Setup's languages** | English, Spanish, Russian, Portuguese (Brazil) and Simplified Chinese, following Windows' display language (`--lang=ru` and so on picks one) |

Speed-ups are meant to leave the image exactly as it was; features that change the image (DLSS, FSR, frame
generation) are options.

## Status and known limits

- Version **1.0.0** of the game is supported; **1.4.3 is a beta** (FAQ: [Which game versions work?](#faq)).
- Made for **Windows 10 / 11**. Linux and Steam Deck through Proton: experimental, not tested on Linux / Steam Deck
  yet ([Linux and Steam Deck](#linux-and-steam-deck-proton-experimental)).
- First visits can stutter while shaders are built (above).
- One graphics device loss was seen during a fight on the developer's PC. It has not been reproduced in several
  long test runs and its cause is unknown. If the game closes with a device-loss error, please send the newest
  `logs\totk-*.log` in a [bug report](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=bug_report.yml).
- Some visual differences from the Switch remain, among them lighting in parts of the Depths. Known problems are in
  [Issues](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues).
- PC settings live in the F10 window and, by section, in the game's own Options screen (System > Options). To hide
  them there, add `env.TOTK_OPTIONS_MENU=0` to the game folder's `player.txt`.
- Setup is not code-signed, so Windows SmartScreen may warn the first time.

## Linux and Steam Deck (Proton, experimental)

**Experimental, not tested on Linux / Steam Deck yet; reports welcome** (a
[GitHub issue](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new?template=bug_report.yml) with
`setup.log` from the install folder and the newest `totk-*.log` from the Proton prefix's
`drive_c/users/steamuser/AppData/Local/nxrecomp/TotK-engine/logs`, the prefix being Steam's
`steamapps/compatdata/<number>/pfx` for the Setup entry). A native Linux build (an AppImage, no Proton) is being tested:
the newest `linux-test` pre-release on the releases page.

Setup and the game are Windows programs; on Linux they run through Steam's Proton. Both notice Proton (or any Wine)
by themselves and take their Linux paths.

1. **Add Setup to Steam.** Download the Setup exe. In Steam (on a Steam Deck: in Desktop Mode) choose
   *Games > Add a Non-Steam Game to My Library > Browse*, set the file type to all files and pick the Setup exe. In
   that entry's *Properties > Compatibility*, tick *Force the use of a specific Steam Play compatibility tool* and pick
   Proton 9.0 or newer, or Proton Experimental.
2. **Install.** Start the entry. Your Linux folders are on drive `Z:` (your home is `Z:\home\<user>`, on a Steam Deck
   `Z:\home\deck`): pick or drop your dump and `prod.keys` from there. The suggested folder is `WeepingOfTheEmpire` in
   your Linux home; any folder on a Linux drive with about 15 GB free works, but not one on drive `C:` (that is the
   entry's Proton prefix, which Steam deletes with the entry). Press **Install**. Proton runs the compilers through
   Wine, so the build takes longer than on Windows; on a Steam Deck keep it plugged in.
3. **Play.** When Setup says Done, press **Play**. To start the game straight from Steam from then on, put `--play`
   in the same entry's *Properties > Launch Options*: Setup then starts the newest build and closes. Clear it to open
   Setup again (an update, or Uninstall).

Keep Setup and the game on that one entry: they then share one Proton prefix, where the game's saves and settings
live (`steamapps/compatdata/<number>/pfx/drive_c/users/steamuser/AppData/Roaming/nxrecomp/TotK-engine` in your Steam
folder) and where the game's automatic update finds Setup's choices. Back that folder up before you remove the entry.
Under Wine the game does not place its threads on chosen cores (Wine does not apply Windows' CPU Sets) and does not
track which game memory was written (that clashes with the Linux graphics drivers), so it spends a little more
processor time per frame than on Windows. Setup puts no shortcut on the desktop there.

## Requirements

- **Your own files:** the game, version 1.0.0 (`.nsp`, `.xci`, `.nsz`, `.xcz` or `.nup`, title `0100F2C0115B6000`) and your `prod.keys` (not for a `.nup`: its files are unencrypted; the firmware's fonts and an update dump still need them);
  for the 1.4.3 beta also your 1.4.3 update dump; optionally your console's firmware, for its Korean and Chinese
  fonts.
- **CPU** with AVX2 (Intel Haswell / AMD Zen or newer) for the build we have tested. Processors with SSE4.2 but
  without AVX2 (Intel Sandy / Ivy Bridge, AMD FX, many Pentium / Celeron) get a compatible build that the Builder picks
  by itself; a player confirmed it on a Pentium G4600 (no AVX); expect low fps on such processors.
  Reports welcome. Setup refuses processors without SSE4.2. **GPU** with a current Vulkan 1.3 driver.
- **RAM:** 8 GB recommended minimum (tested by simulation only), 16 GB recommended; less is allowed but untested.
- **About 15 GB free** on an NTFS drive (the game's data stays in your dump: keep the dump where it is), about 30 GB
  when Setup copies the data out of the dump (a dump on a USB drive, or the window's "Copy the game files"), about
  50 GB with the 1.4.3 update (beta); and an internet connection during the install.

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

**How do I play ultrawide (21:9, 32:9) or 16:10?** F10 > Graphics > Game patches > **Aspect ratio**. Choices: **The window's** (the game takes the shape of your window or monitor: 16:10, 21:9 or 32:9; any other shape keeps 16:9), **16:9** (the game's own picture, with bars on a wider screen), or a fixed **16:10**, **21:9** or **32:9**. The change applies at the next start. From v0.1.9 the default is **The window's**, and a saved 16:9 switches to it once (pick 16:9 again to keep it); before v0.1.9 a 21:9 or 32:9 screen showed the game with bars at the sides until you changed it. If you move or resize the window to another shape, the game tells you to restart for it (F10 > Restart the game now). The first start on a new shape prepares its layout (about 30 s, once); Setup already prepares the shapes of your monitors. Two rows next to it set where the HUD sits on a wide screen and how real-time cutscenes are framed. All rows: [SETTINGS.md](SETTINGS.md#graphics--game-patches).

**Which game versions work?** 1.0.0, and 1.4.3 as a beta. The patches and the recompiled code are tied to each
version's code, so each version is a build of its own; other versions (1.1 to 1.2.1) are not supported. For 1.4.3,
put your own 1.4.3 update dump in Setup's *Update (optional)* row, next to your 1.0.0 dump: Setup builds 1.4.3 in a
folder of its own next to the 1.0.0 build, with its own desktop shortcut (*Weeping of the Empire 1.4.3*); Setup's
**Play** and the main shortcut keep starting 1.0.0. 1.4.3 keeps its own saves: the first time it starts it copies
your 1.0.0 saves once, and later progress in one version does not appear in the other. Its mods go in `mods-v143`
([MODS.md](MODS.md)). It is a beta: it runs at the same frame rates as 1.0.0, but
fewer of the port's PC features and fixes work in 1.4.3 yet. The keyboard and mouse controls (F10 > Controls) should
work there in play as in 1.0.0, the keys that change while Link climbs, glides, swims, rides, uses an ability or aims a bow included (G
should throw what Link carries, or the weapon, as the controller's R does), except the ability keys 4 to 9 (Ultrahand,
Fuse, Ascend, Recall, Autobuild, Camera): they do nothing in 1.4.3 yet, so pick abilities with the ability wheel (hold Tab, or
hold L on the controller). In menus, use the keyboard or a controller: the mouse pointer does not work in
1.4.3's menus yet. Please report what you find.

**Is my keys file safe?** Setup passes it to the build as a path. It is never printed, copied or uploaded, and the
whole build runs on your PC.

**Where are my saves?** `%APPDATA%\nxrecomp\TotK-engine`. Setup keeps them across updates.

**How do I install mods?** Put each mod in a folder of its own inside the `mods` folder next to the game (the game
folder; `%APPDATA%\nxrecomp\TotK-engine\mods` works too), laid out as emulators take them: `<mod>\romfs\...`,
the SD card's `atmosphere\contents\0100F2C0115B6000\romfs\...`, or TKMM's merged output. A whole emulator mods
folder can be copied in as it is (yuzu's `load\0100F2C0115B6000`, Ryujinx's `mods\contents\0100f2c0115b6000`):
each mod in it is listed on its own. Or let the game install a mod as you downloaded it (`.zip`, `.7z`, `.rar` or a
folder): F10 > Mods > Install a mod..., or drop it on `TotK Launcher.exe` or the desktop shortcut. In the game, F10 > Mods
shows them in load order (the top one wins a file two mods change), switches each one on or off, and says why a mod
cannot be used; changes apply at the next start. The resource size table is adjusted by itself, so mods with bigger
files do not need their own. Mods must be for the game version you play: 1.0.0 reads the `mods` folders, a 1.4.3
build (beta) its own `mods-v143` folders (in the same two places); the port's own files (PC button prompts, fixes) stay
over a mod's. No mods ship with the port. The full guide (layouts, F10 > Mods, why a mod is not used):
[MODS.md](MODS.md).

**Do code patches (.ips, .pchtxt) and cheats work?** Yes, in 1.0.0 and in the 1.4.3 beta, each with files made for
its own version: a mod's `exefs` folder (or the SD card's `atmosphere\exefs_patches\<name>`) and its
`cheats\082CE09B06E33A12.txt` (1.4.3: `277178B7DBA1B6D4.txt`; Atmosphere format; a folder holding only that file
is a cheats mod too). The game is recompiled
ahead of time, so a patch that changes code needs the game built again with it: when the mods' code patches change,
the game closes, rebuilds itself with Setup's tools (about 3 minutes; only the changed parts compile again) and starts
again. A patch that changes only data applies at once. A patch that changes code the port already changes (the frame
rate, resolution and other F10 settings) is left out and F10 > Mods says so; use the setting instead. Cheats are
switched one by one in F10 > Cheats (off at first); a cheat that changes values works at once, one that changes the
game's code applies after the rebuild, and one that changes code only while a button is held is not supported yet.

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
