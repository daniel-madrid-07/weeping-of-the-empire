"""A game's RomFS for the build tools, unpacked or not.

A player's install keeps no unpacked folder (setup-speed, 2026-11-01: 304,207 files, 17 GB, 5-11 min to write and a
Defender scan each; the game reads romfs.bin): RomFS(folder) reads the folder when it holds files, else the
extraction's romfs.bin + romfs-index.json beside it (pipeline/extract.py: <ext>/romfs, <ext>/romfs.bin,
<ext>/romfs-index.json). An empty folder counts as none (the runtime creates its mount folder at the first MountRom).

    from romfs_src import RomFS
    rom = RomFS(os.environ['TOTK_ROMFS'])
    rom.read('Pack/ZsDic.pack.zs'); rom.names('Shader')
"""
import json
import os


def _has_files(folder):
    try:
        with os.scandir(folder) as it:
            return any(True for _ in it)
    except OSError:
        return False


class RomFS:
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        self.files = self.image = None
        if _has_files(self.folder):
            return
        index = os.path.join(os.path.dirname(self.folder), 'romfs-index.json')
        if os.path.isfile(index):
            with open(index, encoding='utf-8') as f:
                meta = json.load(f)
            self.image = os.path.join(os.path.dirname(index), meta.get('image', 'romfs.bin'))
            self.files = {e['path'].lstrip('/'): (e['offset'], e['size']) for e in meta['files']}

    def where(self):
        return self.image if self.files is not None else self.folder

    def names(self, under=''):
        """the files' paths ('/'-separated, relative to the RomFS root) under `under` ('' = all), sorted"""
        under = under.strip('/')
        pre = under + '/' if under else ''
        if self.files is not None:
            return sorted(p for p in self.files if p.startswith(pre))
        out = []
        for root, _, fs in os.walk(os.path.join(self.folder, *under.split('/')) if under else self.folder):
            out += [os.path.relpath(os.path.join(root, f), self.folder).replace(os.sep, '/') for f in fs]
        return sorted(out)

    def read(self, rel):
        rel = rel.lstrip('/')
        if self.files is None:
            with open(os.path.join(self.folder, *rel.split('/')), 'rb') as f:
                return f.read()
        if rel not in self.files:
            raise FileNotFoundError(f'{rel}: not in {self.image}')
        off, size = self.files[rel]
        with open(self.image, 'rb') as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise OSError(f'{rel}: short read from {self.image}')
        return data
