# Mods

The port loads RomFS mods (the game's data files) the way emulators and a modded Switch do, plus ExeFS code patches and
Atmosphere cheats made for version 1.0.0, the version the port builds. No mods ship with the port: use mods you
trust, made for 1.0.0.

## Where mods go

Each mod is a folder of its own inside a `mods` folder. Two places are read:

- `mods` next to the game (the folder with the launcher; the launcher creates it), and
- `%APPDATA%\nxrecomp\TotK-engine\mods` (per Windows user).

A mod with the same name in both is used from the game folder's.

A folder whose name starts with `_` or `.` is left out (a quick way to park a mod without deleting it).

## Installing a mod

You can copy a mod's folder in by hand, or let the game do it from the mod as you downloaded it (a `.zip`, `.7z` or
`.rar`, or a folder):

- **F10 > Mods > Install a mod...** (an archive) or **Install a mod folder...** (a folder). It loads at the next start.
  In exclusive fullscreen the game goes to the desktop while the file dialog is open.
- **Drop it on the launcher** (`TotK Launcher.exe` in the game folder, or the desktop shortcut) in Explorer: the mod is
  installed, a box says where it went, and the game starts as usual. Several mods can be dropped at once.

Either way the mod is unpacked and checked first, then copied into the game folder's `mods`, in a folder named
after it; a name already taken gets ` (2)`, ` (3)` and so on. It is not
installed, and the reason is shown, when it has no `romfs`, `exefs` or `cheats` folder, when it is made for another
game, or when it loads code of its own (`subsdk` / `main`, which a recompiled game cannot load). An archive Windows
cannot open: unpack it yourself and use **Install a mod folder...**.

## What a mod folder can look like

Any of these layouts works (names are not case-sensitive):

| Layout | Example |
|---|---|
| Emulator mod folder | `mods\Better Rain\romfs\...` (also `romfslite`) |
| Switch SD card | `mods\Better Rain\atmosphere\contents\0100F2C0115B6000\romfs\...` |
| Title folder | `mods\Better Rain\contents\0100F2C0115B6000\romfs\...` or `...\0100F2C0115B6000\romfs\...` |
| Bare RomFS tree | `mods\Better Rain\Pack\...`, `\Actor\...` (the game's own top folders) |
| Zip unpacked into a folder of its own name | `mods\Better Rain\Better Rain\romfs\...` (one wrapper folder is looked through) |
| TKMM's merged output | its folder, as it writes it |
| Code patches (ExeFS) | `mods\60fps\exefs\*.ips` or `*.pchtxt` (also under `atmosphere\contents\0100F2C0115B6000\`), or the SD card's `mods\60fps\atmosphere\exefs_patches\<name>\*.ips` |
| Cheats | `mods\My cheats\cheats\082CE09B06E33A12.txt` |

A whole emulator mods folder can be copied in as it is: yuzu's `load\0100F2C0115B6000` or Ryujinx's
`mods\contents\0100f2c0115b6000`. Each mod inside it is listed on its own, with the place it came from under its name.

## Order and switches: F10 > Mods

In the game, F10 > Mods lists every mod found, top first:

- **The top mod wins** a file that two mods change. Move a mod with its arrows, or select it and press Ctrl+Up /
  Ctrl+Down (pad: Y / X).
- **On / off** per mod with its checkbox.
- Each mod shows what it does once the game has loaded its files: how many game files it replaces and adds, and how many
  of its files are not used because a mod above it, the port itself, or another game version takes them.
- **Changes apply at the next start** (the tab says so). "Open mods folder" opens the game folder's `mods`.

The order and switches are kept in the game's settings, so they survive updates. New mods come in switched on, below the
ones already known.

## Why a mod says it is not used

| F10 says | Meaning / what to do |
|---|---|
| no romfs, exefs or cheats files | The folder holds none of the layouts above. Check where its `romfs` folder is. |
| the name contains `\|` | Rename the folder (the mod list uses `\|` between names). |
| made for another game (title ...) | Its `contents\<id>` is another game's. |
| an earlier mods folder has a mod with this name | Two folders share a name: the game folder's one is used. |
| code modules (subsdk or main) | Mods that load extra code (plugins) cannot work in a recompiled game: the whole mod stays off. |

Files named for another game version (for example `Mals\USen.Product.121.sarc.zs` from a 1.2.1 mod) are counted as
"for another game version" and are not used: the game never asks for them.

## The resource size table (RSTB)

The game sizes each file's memory from a table, and bigger files from a mod need bigger entries. You do not need an
RSTB tool or a mod's own table: at start the game merges the tables mods bring (each entry the larger) and computes
entries for every file a mod replaces or adds. A table made for another game version still gives the sizes of that
mod's own files.

## Code patches and cheats

Code patches (`.ips`, `.pchtxt`) and Atmosphere cheats are read only from files named for the build you play (the
build id: `082CE09B06E33A12...` for 1.0.0); a mod's files for another version are not used.

- **Code patches:** the game is recompiled ahead of time, so a patch that changes code needs the game built again with
  it. When the mods' code patches change, the game closes at its next start, rebuilds itself with the tools Setup
  installed (about 3 minutes; only the changed parts compile again) and starts again by itself. A patch that changes
  only data applies at once. A patch that changes code the port already changes (frame rate, resolution and the other
  F10 settings) is left out, and F10 > Mods says so: use the setting instead.
- **Cheats:** F10 > Cheats lists the cheats of every mod that is on, all off at first; switch them one by one. A cheat
  that changes values works at once, one that changes the game's code applies after the rebuild, and one that changes
  code only while a button is held is not supported yet. Cheats can break a save: keep a copy.

## What stays the port's

The port serves some files of its own ahead of mods: the PC button prompts (UI and font files while they are drawn for
keyboard and mouse or another pad) and its fixes. A mod that changes the same files loses those files; F10 counts them
as "left to the port's own files".

## When something goes wrong

- Switch a mod off in F10 (or rename its folder to start with `_`) and start again to see if it is the cause.
- The game's log (the newest `totk-*.log` in `%LOCALAPPDATA%\nxrecomp\TotK-engine\logs`) has a `[mods]` line per mod: on, off,
  or why it is not used, and which RomFS folder it serves.
- A mod made for another version of the game can crash it or show wrong text even when its files load: use mods made for
  1.0.0.
