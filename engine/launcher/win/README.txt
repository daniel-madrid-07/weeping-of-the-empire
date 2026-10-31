THE LEGEND OF ZELDA: TEARS OF THE KINGDOM ON PC (Weeping of the Empire)
=======================================================================

Version: commit {commit}; build {build}; object key {key}.

This folder holds no game data. The game is read from what the Builder extracted from your own dump on this PC:
    RomFS:            {romfs}
    Images (NSO):     {img}
    Fixes (romfs):    {overlay}
If you move those folders, change the paths in player.txt (the romfs=, img= and overlay= lines).
Do not share this folder or the work folder: they contain files made from the game.


1. STARTING
-----------
Double-click "TotK Launcher.exe". The launcher starts the game (totk.exe) and waits, without a window, until you
close it.

- The first time, if you have no save, it offers to import one (from an emulator such as yuzu, Eden or Ryujinx, or a
  console backup). You can say no and do it later (section 4).
- Hold Shift while you open the launcher to get the saves menu instead of starting the game.
- At start the game prepares the shaders it already knows (a few seconds, before the world shows) and, in the
  background, every shader of the game: the first time and after each update. It uses all cores on the title
  screen, then continues slowly while you play, so a new place does not stutter when you turn the camera. It takes
  about 2.5 GB in %LOCALAPPDATA%\nxrecomp.


2. THE F10 WINDOW
-----------------
F10 opens the PC settings window, also in the middle of a game (with a controller: L3 + R3). Tabs:
- Graphics: window mode, frame rate, presentation, render resolution, upscaling, sharpening, shadows, DLSS...
- Controls: keyboard and mouse, key bindings ("Add key"), controller buttons, controller (glyphs, rumble, gyro).
- Audio: volume.
- Game: skip the intro, pause when the window loses focus, saves, screenshots (F12) and crash reports.
Changes are saved at once. Some need a restart of the game (the window says so).
Experimental: the same PC settings as rows at the end of the game's own Options screen (System > Options), with
sections and the keys: add the line env.TOTK_OPTIONS_MENU=1 at the end of player.txt.

The frame rate starts at 120 (the original runs at 30). In F10 > Graphics you can choose 30, 60, 90, 120 or
"Unlocked": no limit, up to your monitor's refresh rate but at most 180 fps for now (above about 200 the game's own
audio breaks), with the game running at its right speed.

DLSS frame generation (NVIDIA RTX 40 series or newer, off by default, experimental) does not make the game itself
faster: use it only when the game runs under 60 fps without it, on a monitor of 120 Hz or more, with the frame limit
at 120. It halves the real frames and adds about 16 ms of input delay.

If your monitor runs at more than 60 Hz but the game stays at 60 fps, Windows has the monitor at 59-60 Hz: the game
tells you once. Change it in Settings > Display > Advanced display > Choose a refresh rate.


3. CONTROLS
-----------
Controllers: Xbox (XInput), DualSense, DualShock 4, Pro Controller and Joy-Con. Buttons go by position, as on the
Switch (the bottom one is B); F10 > Controls > Controller can switch that to "by label".

Keyboard and mouse (every key can be changed in F10 > Controls > Key bindings):
    Move ................ W A S D            Camera .............. mouse (or the arrow keys)
    Jump ................ Space              Interact / talk ..... E
    Attack .............. left click (hold: charged attack; with the weapon away, the click draws it and attacks)
    Shield and lock on .. hold right click   Perfect guard ....... right click held, then E just before the hit
    Dodge ............... right click held, a direction (A / D / S) and Space
    Sprint .............. Shift              Crouch .............. Ctrl
    Weapons, shields, bows, materials: 1, 2, 3, C open their strip; choose with the wheel or the mouse, close with a
                          click, the same key or Esc (the marked one is equipped)
    Put away / draw ..... Q                  Bow ................. F (left click draws and shoots)
    Abilities ........... 4 to 9; Tab: the last one (hold: the wheel)
    Throw ............... G                  Binoculars .......... V
    Whistle ............. X                  Pause ............... Esc
    Inventory ........... I                  Map ................. M
    Journal ............. J
    Menus: Q / E change tabs, Enter or a click confirms, Esc goes back.
    Horse: Shift gallops, Space jumps, E gets off, R soothes. Paraglider: Space closes it, G dives.

PC keys:
    F10 ............ the settings window
    F11 or Alt+Enter fullscreen / window
    F12 ............ screenshot (in Pictures\Screenshots), also with the F10 window open
    F3 ............. frame counter
    F9 ............. performance graph
    F2 ............. on the title screen: skip the intro or not (also the - button)

Mouse: click in the window to let it move the camera (that click does not attack). F10 or Alt+Tab free the pointer;
in the game's menus the pointer shows by itself.


4. SAVES AND BACKUPS
--------------------
Your save is in
    %APPDATA%\nxrecomp\TotK-engine\userdata\save
(paste that path into Explorer's address bar). The game saves by itself, as on the console.

Before each start the launcher copies the save to
    %APPDATA%\nxrecomp\TotK-engine\backups
and keeps the 10 newest automatic copies (other copies are never deleted).

Saves menu (Shift while opening the launcher):
- Import a save: from yuzu and its forks (Eden...), Ryujinx, or any folder (for example a JKSV backup from your
  console's SD card). The current save is backed up first.
- Restore a backup (the current save is backed up first).
- Make a backup now.
This is version 1.0.0 of the game: a save from a later version (1.1, 1.2...) may not load.


5. IF SOMETHING GOES WRONG
--------------------------
- If the game closes after a crash, the launcher tells you where the report is. Crash reports and minidumps are in
  %LOCALAPPDATA%\nxrecomp\TotK-engine\crashes, the game's logs in %LOCALAPPDATA%\nxrecomp\TotK-engine\logs, the
  launcher's log in %LOCALAPPDATA%\nxrecomp\TotK-engine\launcher.log.
- The settings are in %APPDATA%\nxrecomp\TotK-engine\settings.ini. Delete that file and the launcher puts this
  folder's defaults back at the next start.
- If loads (Continue, fast travel) take much longer than a few seconds, an antivirus may be scanning each of the
  game's thousands of files as they are read. With Microsoft Defender you can exclude the RomFS folder above and this
  folder: Windows Security > Virus & threat protection > Manage settings > Exclusions > Add an exclusion > Folder.
  Exclude only those two folders, never a whole drive.
- On CPUs with two kinds of cores (Intel 12th generation and newer, Core Ultra) or two chiplets (12- and 16-core
  Ryzen 9), the game puts its own, render and audio threads on the fast cores (or the chiplet with more cache) and
  background work on the others. If you see stutters there, add this line at the end of player.txt and start the game
  again (delete it to undo):
      env.TOTK_CPU_PLACE=0
