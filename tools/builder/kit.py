"""The download kit: everything Setup and the build download, in one folder (nxr-kit) another PC can install from
without internet.

Setup --download-kit <dir> makes <dir>/nxr-kit on a PC with internet: Setup's own downloads, then builder.py
--download-kit, with NXR_KIT_MAKE set, so every download below is also recorded. Setup beside an nxr-kit folder (or
--kit <dir>) runs the build with NXR_KIT set: each download looks in the kit first and takes a file only when its
sha256 is the one recorded (and the one pinned, where the code pins one); anything else goes to the network as before,
and a failure there says it was not in the kit either.

Layout (all made by the player's own Setup, for their own use: Microsoft's libraries are not ours to publish):
    kit.json        {"format": 1, "setup": <tag>, "complete": bool, "files": [{name, url, sha256, size, path}],
                     "source": {"path": "src.git", "tag": <tag>}}
    files/<sha256>  each download, by its content (the same file is kept once)
    wheels/         the Python packages (pip --find-links), each listed in files with url "wheel:<file name>"
    src.git         the source at Setup's tag (a bare git repository, one commit deep)
"""
import hashlib
import json
import os
import shutil

FORMAT = 1


def making():
    return os.environ.get('NXR_KIT_MAKE') or None


def using():
    k = os.environ.get('NXR_KIT')
    return k if k and os.path.isfile(os.path.join(k, 'kit.json')) else None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def load(kit):
    try:
        with open(os.path.join(kit, 'kit.json'), encoding='utf-8') as f:
            doc = json.load(f)
        if isinstance(doc, dict) and isinstance(doc.get('files'), list):
            return doc
    except (OSError, ValueError):
        pass
    return {'format': FORMAT, 'complete': False, 'files': []}


def save(kit, doc):
    p = os.path.join(kit, 'kit.json')
    with open(p + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=1)
        f.write('\n')
    os.replace(p + '.tmp', p)


def _put(doc, entry):
    doc['files'] = [e for e in doc['files'] if e.get('url') != entry['url']] + [entry]


def record(path, url, name, sha=None):
    """Making a kit: the downloaded file at path goes into it (files/<sha256>, once) with its name and source url"""
    kit = making()
    if not kit or not os.path.isfile(path):
        return
    s = sha or sha256(path)
    rel = 'files/' + s
    dst = os.path.join(kit, 'files', s)
    if not (os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(path)):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(path, dst + '.tmp')
        os.replace(dst + '.tmp', dst)
    doc = load(kit)
    _put(doc, {'name': name, 'url': url, 'sha256': s, 'size': os.path.getsize(path), 'path': rel})
    save(kit, doc)


def record_bytes(data, url, name):
    """Making a kit: a small download held in memory (Microsoft's manifests), as record does with a file"""
    kit = making()
    if not kit:
        return
    s = hashlib.sha256(data).hexdigest()
    dst = os.path.join(kit, 'files', s)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(dst + '.tmp', dst)
    doc = load(kit)
    _put(doc, {'name': name, 'url': url, 'sha256': s, 'size': len(data), 'path': 'files/' + s})
    save(kit, doc)


refused = []   # (what the kit held damaged: said once each, never used)


def find(url, sha=None, name=None, say=print):
    """The kit's copy of a download (by its pinned sha256, else by its url), checked against the recorded sha256 and
    the pinned one; None when there is no kit, the kit lacks it, or its copy is damaged (refused, said)"""
    kit = using()
    if not kit:
        return None
    doc = load(kit)
    e = next((e for e in doc['files'] if sha and e.get('sha256') == sha), None) or \
        next((e for e in doc['files'] if e.get('url') == url), None)
    if not e:
        return None
    p = os.path.join(kit, *e.get('path', '').split('/'))
    want = sha or e.get('sha256')
    if not os.path.isfile(p) or e.get('sha256') != want or sha256(p) != want:
        if p not in refused:
            refused.append(p)
            say(f'{name or url}: the download kit\'s copy ({p}) is damaged or not the one pinned: not used')
        return None
    return p


def take(url, dest, sha=None, name=None, say=print):
    """dest from the kit (find); True when it came from there"""
    p = find(url, sha, name, say)
    if not p:
        return False
    os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
    shutil.copyfile(p, dest + '.kit')
    os.replace(dest + '.kit', dest)
    say(f'{name or os.path.basename(dest)}: from the download kit')
    return True


def wheels(say=print):
    """The kit's wheels folder for pip --find-links, when every wheel in it is the one recorded; else None (said)"""
    kit = using()
    d = os.path.join(kit, 'wheels') if kit else None
    if not d or not os.path.isdir(d):
        return None
    sums = {e['url'][len('wheel:'):]: e.get('sha256') for e in load(kit)['files'] if e.get('url', '').startswith('wheel:')}
    for n in os.listdir(d):
        if n.endswith('.whl') and sums.get(n) != sha256(os.path.join(d, n)):
            say(f'the download kit\'s Python package {n} is damaged or unlisted: the kit\'s packages are not used')
            return None
    return d


def record_wheels(d):
    """Making a kit: every wheel pip downloaded into d (the kit's wheels folder) listed with its sha256"""
    kit = making()
    if not kit:
        return
    doc = load(kit)
    for n in sorted(os.listdir(d)):
        if n.endswith('.whl'):
            p = os.path.join(d, n)
            _put(doc, {'name': n, 'url': 'wheel:' + n, 'sha256': sha256(p), 'size': os.path.getsize(p),
                       'path': 'wheels/' + n})
    save(kit, doc)


def missing_note():
    """For a download that failed with a kit in use: it was not in the kit either, and what to do"""
    kit = using()
    return (f' It is not in the download kit ({kit}) either: make the kit again with Setup --download-kit on a PC with '
            'internet, or connect this PC.') if kit else ''
