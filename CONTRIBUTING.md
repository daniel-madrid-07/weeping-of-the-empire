# Contributing

Thanks for helping. Reports are as welcome as code: a [compatibility report](COMPATIBILITY.md) for a PC nobody has
listed yet, or a bug report with the game's log, often saves a day of guessing.

## Rules

- **No game material, ever.** No game code, data, assets, keys, firmware, decompiled game source or built game
  folders in issues, pull requests or commits. Instruction words and file names from the game are matched by hash,
  as the existing patches do. Requests for files or builds are closed without reply.
- **One change per pull request**, with a sentence on what it fixes and how you checked it (the build, the place in
  the game, your PC).
- **Performance changes must not change the image.** Say how you compared before and after (same place, same
  settings) and give the numbers. Anything that trades image quality for speed is an option, off by default.
- **Bugs and reports:** use the issue forms. The forms ask for F10's **Copy system info** text and the game's log
  (`%LOCALAPPDATA%\nxrecomp\TotK-engine\logs\totk-*.log`; the paths in it can show your Windows user name).
- **Licence:** by contributing you agree that your code is released under this repository's [licence](LICENSE).
  Code taken from another project keeps its licence and goes in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
  one line per file group.

## Building from source

Everything is built on your PC from your own dump and keys; nothing of the game is in this repository.

**The easy way:** Setup (the release's `Setup.exe`) builds from this repository's source at its release tag, fetches
the compilers itself and tells you what it needs. Its folder holds the same source and tools as a clone.

**From a clone** (Windows 10 / 11 x64, an NTFS drive):

1. Install Python 3.11 or later with `zstandard pillow fonttools numpy cryptography lz4 skia-pathops` (the build
   makes its own Python environment for the rest, capstone among them), git, LLVM
   (this repository's releases use clang 23.1.1), CMake, Ninja (`winget install LLVM.LLVM Kitware.CMake
   Ninja-build.Ninja`), Visual Studio 2022 Build Tools 17.14 or later with the C++ workload, and the Vulkan SDK
   (releases use 1.4.357.0). Clone the repository with git: the build needs a clone, not a ZIP download.
2. Build:

   ```
   py tools/builder/builder.py <your 1.0.0 dump (.nsp / .xci / .nsz / .xcz)> <your prod.keys> [<firmware folder or .zip>] --work <work folder> --out <game folder>
   ```

   It runs eight steps (extract, RomFS, images, patches, build, setting sets, overlay, package); a step whose output
   is current is skipped, so running it again continues where it stopped. The first build takes about 25 to 45
   minutes on a recent 8-core CPU, needs 8 GB of RAM as the recommended minimum (16 GB recommended; less is allowed but untested) and about 15 GB free, and the internet
   once. `--dry-run` lists the steps; `--cpu v2` builds for CPUs without AVX2. The missing-tool messages say what to
   install.
3. Play from `<game folder>` with its launcher, as from Setup's folder.

**After a change:** run the same `builder.py` command again. Steps whose inputs did not change are skipped, and the
build step compiles only what your change touched and reuses every other object (minutes, not the first build's half
hour). The launcher alone: `python engine/launcher/win/build.py` (a few seconds).

**Tests that run without the game:** `python engine/launcher/win/build.py test` (the launcher, with a fake game),
and `py tools/builder/test_toolchain.py`, `test_resume.py`, `test_tidy.py`, `test_dump_check.py`. With a work
folder from a build (your own extracted game): `python -m games.totk.tests.run_all --work <work folder>` checks the
game patches on your copy of the code.

## Code style

- Match the file you are in: its naming, comment density and idioms. C++ is C++20, 4 spaces, lines up to about 120
  characters, LF line endings (Python writes files with `newline='\n'`).
- A file starts with a one-line comment saying what it is for. Comments say why, not what the next line does.
- Commit subjects: `<type>(<area>): <what>`, type one of feat, fix, perf, refactor, docs, test, chore; the body says
  why and how it was checked.
- Every commit builds and links on its own, and stays small enough that a bisect lands on one change.
- A new settings row gets a one-line help text for players in `engine/runtime/pc_help.cpp` (F10 shows it, and the
  settings guide is made from it). Say in the pull request when you add or change one.

## Pull requests

1. Fork, branch from `main`, keep it to one change.
2. Describe what it fixes, why, and how you checked it: which PC, which place in the game, before / after numbers for
   anything about speed.
3. Every pull request is reviewed by reading the change; a change to the renderer also gets a graphics review. Expect
   questions about edge cases and other GPUs: they are not a refusal.
4. Issues labelled **good first issue** are small and self-contained: a good place to start. Say in the issue that
   you are on it, so two people do not do the same work.
5. **Pull requests are not merged here**: this repository is exported from the maintainers' own one, and the next
   export would undo a merge. An accepted change is applied there with you as the commit's author (your GitHub noreply
   address), the pull request is closed with "applied in <commit>, ships in vX", and the release that ships it credits
   you: its commit here names you as a co-author and its notes by your GitHub handle.
