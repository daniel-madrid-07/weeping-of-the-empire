"""The game key a totk.exe was linked against, written at its end (fps360 + LEAD 11-03 12:52: a slot exe linked from dev
key 2756a946 while player.txt said 11637b05843c7382 ran the game 2.7-3.7x; nothing could tell). build.py's runtime link
appends b'\\0nxr-key=<key>\\0' after the last section (a PE or ELF loader ignores bytes past its sections), and
package.py / the boot and gate tools refuse an exe whose tag is not the key they package or boot.
    python tools/engine/exekey.py <exe>   -> prints the key, or 'untagged' (exit 1)"""
import os, re, sys

TAG = b'nxr-key='
PTAG = b'nxr-patches='   # (fps360: the key's game-patch features joined by '+', so the runtime can see e.g. realtime)
TAIL = 4096   # (the tag is the file's last bytes; a little slack for a tool that appends after it)


def read(path):
    """the key the exe was linked against, or None (no tag: a link from before 11-03, or not a build.py exe)"""
    with open(path, 'rb') as f:
        f.seek(max(0, os.path.getsize(path) - TAIL))
        tail = f.read()
    m = re.findall(rb'\0' + re.escape(TAG) + rb'([0-9a-f]{16})\0', tail)
    return m[-1].decode() if m else None


def read_patches(path):
    """the game-patch features the exe's key was generated with (a list), or None (no tag, or a key tag without them)"""
    with open(path, 'rb') as f:
        f.seek(max(0, os.path.getsize(path) - TAIL))
        tail = f.read()
    m = re.findall(rb'\0' + re.escape(TAG) + rb'[0-9a-f]{16}\0(' + re.escape(PTAG) + rb'([A-Za-z0-9_+]*)\0)?', tail)
    if not m or not m[-1][0]:
        return None
    return m[-1][1].decode().split('+') if m[-1][1] else []


def tag(path, key, features=None):
    """append the key tag, and the key's game-patch features after it when given (once: a second call with the same key
    and features leaves the file as it is)"""
    if not re.fullmatch(r'[0-9a-f]{16}', key or ''):
        raise ValueError(f'not a game key: {key!r}')
    feats = None if features is None else [f for f in features if re.fullmatch(r'[A-Za-z0-9_]+', f)]
    if features is not None and len(feats) != len(features):
        print(f'[exekey] WARNING: features not written to {path} (not identifiers): '
              f'{[f for f in features if f not in feats]}', file=sys.stderr, flush=True)
    if read(path) == key and (feats is None or read_patches(path) == feats):
        return
    rec = b'\0' + TAG + key.encode() + b'\0'
    if feats is not None:
        rec += PTAG + '+'.join(feats).encode() + b'\0'
    with open(path, 'ab') as f:
        f.write(rec)


def refuse(path, key):
    """'' when the exe was linked for key, else why it must not be packaged / booted with it"""
    got = read(path)
    if got == key:
        return ''
    if got is None:
        return f'{path} carries no key tag (linked before 11-03, or not by build.py): relink it against {key}'
    return f'{path} was linked against key {got}, not {key}: relink it (build.py runtime --key {key} --patches ...)'


if __name__ == '__main__':
    k = read(sys.argv[1])
    pf = read_patches(sys.argv[1])
    print((k or 'untagged') + ('' if pf is None else ' patches=' + '+'.join(pf)))
    sys.exit(0 if k else 1)
