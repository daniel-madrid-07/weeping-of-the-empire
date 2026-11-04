# Compatibility

What players have reported on their PCs, newest version first. It only lists what someone measured or saw. A report
for your PC helps others decide, and helps us fix what fails: use the **Compatibility report** form on the
[issues page](https://github.com/daniel-madrid-07/weeping-of-the-empire/issues/new/choose). It takes two minutes:
paste the text F10's **Copy system info** button copies, and say where you stood and what the frame-rate readout showed (F3 shows it; set F10's frame-rate readout to Detailed for the 1 % low, or use the F9 overlay).

Requirements are in the [README](README.md#requirements). In short: a GPU with a current Vulkan 1.3 driver; a CPU
with AVX2 for the build we have tested (processors with SSE4.2 but without AVX2 get a compatible build by themselves);
8 GB of RAM as the recommended minimum (tested by simulation only), 16 GB recommended; less is allowed but untested.

Legend: **Plays** = starts and plays through what was tested. **Problems** = plays, with the listed issues.
**No** = does not start or does not get into the game. Versions are the port's (v0.1.x).

## Reports

| GPU | CPU | RAM | Version | Result | Frame rate (where, settings) | Reports |
|---|---|---|---|---|---|---|
| RTX 5080 | Ryzen 7 9800X3D | | v0.1.7 | Plays | Kakariko route at 2560x1440: median 178.5 fps, 1 % low 120.7 (unlocked); 116-120 at a 120 cap; 114-119 at a 120 cap with a 3840x2160 render | developer's PC, v0.1.7 notes |
| RTX 5070 Ti | Ryzen 7 5800X3D | 32 GB | v0.1.6.1 | Plays (DLAA blur fixed in v0.1.8) | 2560x1440, render x1.6, DLAA | #37 |
| RTX 5070 Ti | Core i5-12600K | 48 GB | v0.1.7 | Plays | | #36 |
| RTX 5070 | Ryzen 7 5800X3D | 32 GB | v0.1.6.1 | Plays (red grass at LOD bias +2 fixed in v0.1.8) | 2560x1440 | #20 |
| RTX 4090 | Core i7-13700K | 32 GB | v0.1.8 | Problems: DLSS / DLAA streaks on terrain while panning | 3840x2160, DLSS from 3200x1800 | #46 |
| RTX 4070 Ti | Ryzen 7 5800X | 32 GB | v0.1.6.1 | Plays | 2560x1440 | #29 |
| RTX 4060 | Ryzen (5600X class) | 32 GB | v0.1.7 | Plays (audio static and an inventory crash fixed in v0.1.8) | | #28, #38, #42 |
| RTX 3080 | Ryzen 7 9800X3D | 32 GB | v0.1.6.1 | Plays (audio static fixed in v0.1.8; mist flicker and particle squares were expected fixed in v0.1.8, and #19 was seen again on an RX 7900 XTX) | 1920x1080, render x1.5; 60 cap | #17, #18, #19 |
| GTX 1660 | Ryzen 7 5700X | 32 GB | v0.1.6.1 | Plays | steady 60 fps at 2560x1440 (render 1600x900) | #30 |
| GTX 1660 Ti (laptop) | Core i5-1135G7 | 16 GB | v0.1.6 | Problems: over 5 GB of VRAM at 4K, under 10 fps after Sky / Surface teleports | 30+ fps at 3840x2160 | #16 |
| GTX 1650 Super | Ryzen 5 3600 | 8 GB | v0.1.6.1 | Plays since v0.1.8 (missing text fixed); 2-4 fps for ~5 min on the first start while shaders build | ~140 fps after that | #22 |
| RX 9070 XT | Ryzen 7 5800X3D | 32 GB | v0.1.7 | Plays (movie fixes in v0.1.7) | 3840x2160, render x1.5-2, HDR10 | #13, #24, #35 |
| RX 7900 XTX | | | v0.1.8 | Problems: squares around particles with render "Follow the window height"; FSR crashed (no longer crashes since v0.1.8.1; FSR may stay off on some GPUs) | 3840x2160 | #19, #45 |
| RX 6800 | Ryzen 5 5600X | 32 GB | v0.1.7 | Problems: some stutter, grass does not stay cut, a fine mesh on Depths roots | 3840x2160: Depths 35-45, Great Sky Island 20-30, Surface 40-50; shrines 60 (v0.1.6) | #9, #16, #41 |
| RX 6600 | Ryzen 5 4500 | 16 GB | v0.1.7 | Plays | 1920x1080, 60 cap | #25, #18, #40 |
| RX 550 | Pentium G4600 (no AVX) | | v0.1.7 | Plays (the build for CPUs without AVX) | low, no number given | v0.1.7 notes |
| Radeon Vega 7 (Ryzen 7 4700U) | Ryzen 7 4700U | 16 GB | v0.1.8 | Plays since v0.1.8 (the reporter confirmed it; frame times still uneven); unplayable on v0.1.5 (1-2 s per frame, shader preparation stuck) | 15-20 fps | #23 |
| Radeon graphics of the Ryzen 7 9800X3D | Ryzen 7 9800X3D | | | Plays, low frame rate | | developer's PC |
| Arc B580 | Ryzen 7 7700 | | v0.1.8 | Problems: crash at the title in Intel's shader compiler (a fix in v0.1.9, not yet confirmed on the reporter's PC, #34), white patches on water and in caves; FSR crashed (no longer crashes since v0.1.8.1; FSR may stay off on some GPUs) | ~2560x1440 | #34, #39, #45 |

Not reported yet: GTX 900 / GTX 10 series, Intel Iris Xe and Arc A-series, Steam Deck, AMD RX 400 / 500 at 1080p
with a number. Reports for these are especially welcome.

## Known problems by vendor (open issues)

- **All GPUs:** endless loading at the In-Isa shrine on one PC (#47). (FSR crashing on v0.1.8, #45: no longer crashes since v0.1.8.1, but FSR may stay off on some GPUs.)
- **NVIDIA:** DLSS / DLAA terrain streaks while panning (#46). High VRAM use at 4K on 6 GB cards (#16).
- **AMD:** squares around particles at "Follow the window height" on RX 7900 XTX (#19); fine mesh on Depths roots
  and a gloom effect near gloom (#41).
- **Intel Arc:** white patches on water and in caves (#39). (For the crash at the title in the driver's shader compiler,
  #34, v0.1.9 has a fix: please tell us if it still happens.)
- **Setup:** a drive formatted FAT32 is refused (use NTFS or exFAT); on some Core i7 / i9 13th and 14th gen PCs Setup
  crashed in Python, which pointed at the CPUs' known instability (#44).

This page is kept current from the reports.
