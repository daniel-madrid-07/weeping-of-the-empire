"""The shader translator's identity: a 64-bit hash of the renderer's sources (engine/runtime/nvn, every file, by relative
path and content). build.py's small_link compiles it into exec.c (-DNXR_XLATE_ID), whose shader-cache keys
(cache/precompile_cold.key, cache/archive_warm.key) used the exe's size and time: every relink, a fix anywhere in the
runtime or the game code, re-warmed the whole shader archive (v0.1.8.1's update: 184 of 335 s). The translation of a
program is code under nvn/ (the GLSL / SPIR-V caches are keyed by their own source hashes, the pipeline cache by the
driver), so a build whose nvn/ is unchanged keeps the warm record. Setup's step 9 (builder.py) keys on it too.

    python tools/engine/xlate_id.py [engine dir]   (prints it)
"""
import hashlib, json, os, sys


def xlate_id(engine, variant=None):
    """the hash of <engine>/runtime/nvn (paths with / separators, sorted, then each file's bytes), as an int; variant: the
    renderer's compile flags that change its output (--march, PGO, LTO: FMA contraction folds float immediates into the
    GLSL differently), mixed in when given"""
    root = os.path.join(engine, 'runtime', 'nvn')
    files = []
    for d, dirs, names in os.walk(root):
        dirs[:] = [x for x in dirs if x != '__pycache__']
        files += [os.path.join(d, n) for n in names if not n.endswith(('.pyc', '.o', '.obj'))]
    if not files:
        raise FileNotFoundError(f'{root}: no renderer sources')
    h = hashlib.sha256()
    for p in sorted(files, key=lambda p: os.path.relpath(p, root).replace('\\', '/')):
        rel = os.path.relpath(p, root).replace('\\', '/')
        with open(p, 'rb') as f:
            data = f.read().replace(b'\r\n', b'\n')   # (a checkout's line ends do not change the translator)
        h.update(rel.encode() + b'\0' + len(data).to_bytes(8, 'little') + data)
    if variant:
        h.update(b'\0variant\0' + json.dumps(variant, sort_keys=True).encode())
    return int.from_bytes(h.digest()[:8], 'little')


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    print(f'{xlate_id(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "engine")):016x}')
