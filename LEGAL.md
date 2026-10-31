# Legal notes

- **No game content.** This repository holds no code, data, assets, text, shaders, keys or firmware from any console
  or game, and no decompiled game code. What it holds about the game is what a patch needs to find its place:
  function addresses, offsets, structure offsets, the names of the game's own files, functions and UI panes those
  patches touch, and the values a layout edit checks before it writes. The game's instructions a patch replaces are
  checked by a SHA-256 of them, never stored: they are read from your own dump. Where a hook changes what a game
  function does, our code describes and redoes that behaviour itself. Our own menu labels use the names the game gives
  its controls and abilities, so players recognise them. `.gitignore` refuses the game's file formats.
- **Your own copy only.** The builder needs what you dump from your own console: the game (version 1.0.0), the keys
  file and, optionally, the firmware (only its Korean and Chinese fonts are used; without it a Windows font stands
  in). On your PC it decrypts and extracts the game with your keys,
  recompiles its code and builds the playable folder; everything it makes stays in the work folder next to this
  repository. The keys file is passed on as a path: it is never printed, copied or uploaded.
- **The optimisation profile (optional).** The builder downloads, from this project's releases, a profile for
  profile-guided optimisation that we made by running the game on our own PC (LLVM's profile format). It holds function
  names, a hash of each function's control flow, how many times each of its branches ran, which function each indirect
  call reached and how often, and the sizes of some memory copies and how often each came up. The names are of two
  kinds: the recompiled game's functions, named by their offset in the game's program (`f_7a83e4`), and the functions of
  our own runtime and the libraries it uses, as named in their source. It holds no code, data, text or assets from the
  game. A small record comes with it: whether it covers our runtime too, and the SHA-256 hashes of the files it was
  trained on (the game's decrypted program files among them), so the builder can say which changed since. The builder
  checks both files' SHA-256 before using them, and builds without the profile when it is missing, fails the check or
  you opt out (`--no-pgo`); the game is then somewhat slower.
- **Do not share builds.** The folders the builder makes, and the work folder, contain code and data recompiled or
  extracted from the game. Do not share or upload them.
- **Third-party code** keeps its licence: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists what is included,
  where, and under which licence, and what the build downloads (the NVIDIA DLSS SDK).
- **Trademarks.** Nintendo Switch and The Legend of Zelda are trademarks of Nintendo. NVIDIA, DLSS and RTX are
  trademarks of NVIDIA Corporation. Xbox and PlayStation are trademarks of their owners; the button glyphs are
  Kenney's CC0 drawings.
- This project is not affiliated with, endorsed by or connected to Nintendo or any other company.
- Our own code is under the licence in [LICENSE](LICENSE).
