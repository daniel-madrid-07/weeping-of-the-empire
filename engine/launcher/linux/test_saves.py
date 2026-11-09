"""launcher_linux.py's save import on fake folder trees (no game, Linux): test_saves.c's cases (the emulators' saves
found, an import failing at each step, an interrupted import repaired, backups and their pruning, links not followed,
read-only folders) in the Linux folders (XDG, Flatpak); the import with the converter (test_savever.c's test_import,
and a conversion end to end on synthetic tables); --saves / --import-save as 'TotK Launcher' passes them; the game
folder package.py makes; and the AppImage window's Saves helpers through that folder's launcher. Seconds.

    python3 -I engine/launcher/linux/test_saves.py
"""
import contextlib
import glob
import importlib.util
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import launcher_linux as ll  # noqa: E402
import savever as sv  # noqa: E402
import test_savever as tsv  # noqa: E402  (its synthetic tables and saves; its tests run only as a script)

fails = []
TOTK = '0100F2C0115B6000'


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + ('' if ok else f' {detail}'), flush=True)
    if not ok:
        fails.append(name)


def write(path, data=b'', minutes_ago=0):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data.encode() if isinstance(data, str) else data)
    if minutes_ago:
        t = time.time() - 60 * minutes_ago
        os.utime(path, (t, t))


def fake_slot(d, slot, text, minutes_ago=1):
    """<d>/slot_NN/progress.sav holding text, written minutes_ago, and a read-only caption.sav (as from an SD card)"""
    write(os.path.join(d, 'slot_%02d' % slot, 'progress.sav'), text, minutes_ago)
    cap = os.path.join(d, 'slot_%02d' % slot, 'caption.sav')
    write(cap)
    os.chmod(cap, 0o444)


def slot0(d):
    """the text of <d>/slot_00/progress.sav (None: none)"""
    try:
        with open(os.path.join(d, 'slot_00', 'progress.sav'), 'rb') as f:
            return f.read().decode('latin-1')
    except OSError:
        return None


def run_main(*argv):
    """launcher_linux.main(argv): (exit code, what it printed)"""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = ll.main(list(argv))
    ll._LOG, ll._ECHO = None, True
    return rc, out.getvalue()


@contextlib.contextmanager
def home_env(home):
    """os.environ with HOME and the XDG folders in home (what the CLI and the window's launcher read)"""
    saved = dict(os.environ)
    os.environ.update(HOME=home, XDG_DATA_HOME=os.path.join(home, '.local', 'share'),
                      XDG_CONFIG_HOME=os.path.join(home, '.config'), XDG_CACHE_HOME=os.path.join(home, '.cache'))
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)


def emulators(home):
    """test_saves.c's emulator tree in the Linux folders, plus Flatpak ones and a link; the paths of the TotK saves"""
    share, config, flat = (os.path.join(home, *p) for p in (('.local', 'share'), ('.config',), ('.var', 'app')))
    yuzu = os.path.join(share, 'yuzu', 'nand', 'user', 'save', '0000000000000000')
    p = dict(yuzu=os.path.join(yuzu, 'A' * 32, TOTK),
             eden=os.path.join(share, 'eden', 'nand', 'user', 'save', '0000000000000000', 'B' * 32, TOTK),
             ryujinx=os.path.join(config, 'Ryujinx', 'bis', 'user', 'save', '0000000000000001', '0'),
             nxrecomp=os.path.join(share, 'nxrecomp', 'TotK', 'profile', 'save', '0123456789abcdef'),
             citron_flatpak=os.path.join(flat, 'org.example.Citron', 'data', 'citron', 'nand', 'user', 'save',
                                         '0000000000000000', 'D' * 32, TOTK),
             ryujinx_flatpak=os.path.join(flat, 'org.example.Ryujinx', 'config', 'Ryujinx', 'bis', 'user', 'save',
                                          '0000000000000003', '1'))
    fake_slot(p['yuzu'], 0, 'yuzu', 30)
    fake_slot(p['eden'], 0, 'eden', 10)
    fake_slot(p['eden'], 1, 'eden1', 12)
    fake_slot(os.path.join(yuzu, 'C' * 32, '0100000000010000'), 0, 'other game', 1)
    fake_slot(p['ryujinx'], 0, 'ryujinx', 5)
    ryu = os.path.dirname(os.path.dirname(p['ryujinx']))
    write(os.path.join(ryu, '0000000000000001', 'ExtraData0'), ll.TOTK_TITLE_ID.to_bytes(8, 'little'))
    fake_slot(os.path.join(ryu, '0000000000000002', '0'), 0, 'ryujinx other game', 2)
    write(os.path.join(ryu, '0000000000000002', 'ExtraData0'), (0x0100000000010000).to_bytes(8, 'little'))
    fake_slot(p['nxrecomp'], 0, 'nxrecomp', 60)
    fake_slot(p['citron_flatpak'], 0, 'citron', 20)
    fake_slot(p['ryujinx_flatpak'], 0, 'ryujinx flatpak', 40)   # (no ExtraData0, only the working copy 1)
    linked = os.path.join(home, 'elsewhere', 'E' * 32)
    fake_slot(os.path.join(linked, TOTK), 0, 'linked', 3)
    suyu = os.path.join(share, 'suyu', 'nand', 'user', 'save', '0000000000000000')
    os.makedirs(suyu)
    os.symlink(linked, os.path.join(suyu, 'E' * 32))   # (a link on the way: never followed)
    return p


def test_find(t):
    home = os.path.join(t, 'home')
    p = emulators(home)
    found = ll.find_emulator_saves({'HOME': home})
    check('emulators: TotK saves only, newest first', [(c['source'], c['path']) for c in found] ==
          [('Ryujinx', p['ryujinx']), ('eden', p['eden']), ('citron', p['citron_flatpak']), ('yuzu', p['yuzu']),
           ('Ryujinx', p['ryujinx_flatpak']), ('nxrecomp', p['nxrecomp'])], [(c['source'], c['path']) for c in found])
    eden = next((c for c in found if c['source'] == 'eden'), {})
    check('emulators: slots and the user folder', (eden.get('slots'), eden.get('user', '')[:4]) == (2, 'BBBB'), eden)
    other = os.path.join(t, 'xdg')
    fake_slot(os.path.join(other, 'data', 'eden', 'nand', 'user', 'save', '0000000000000000', 'F' * 32, TOTK), 0, 'x')
    found = ll.find_emulator_saves({'HOME': home, 'XDG_DATA_HOME': os.path.join(other, 'data'),
                                    'XDG_CONFIG_HOME': os.path.join(other, 'config')})
    check('emulators: $XDG_DATA_HOME / $XDG_CONFIG_HOME followed', sorted(c['source'] for c in found) ==
          ['Ryujinx', 'citron', 'eden'] and next(c for c in found if c['source'] == 'eden')['path'].startswith(other))
    many = os.path.join(t, 'many')
    for i in range(20):
        fake_slot(os.path.join(many, '.local', 'share', 'eden', 'nand', 'user', 'save', '0000000000000000',
                               '%032X' % i, TOTK), 0, 'eden %d' % i, i + 1)
    check('the menu lists at most 16, the newest', [c['user'] for c in ll.find_saves(None, {'HOME': many})] ==
          ['%032X' % i for i in range(16)])

    # a layout like EmuDeck's: the save folders are links into ~/Emulation. A link on the way is followed, a link met
    # while listing a folder is not (suyu's above)
    deck = os.path.join(t, 'deck')
    real = os.path.join(deck, 'Emulation', 'saves')
    fake_slot(os.path.join(real, 'yuzu', '0000000000000000', 'A' * 32, TOTK), 0, 'deck yuzu', 5)
    fake_slot(os.path.join(real, 'ryujinx', '0000000000000001', '0'), 0, 'deck ryujinx', 6)
    for link, target in ((('.local', 'share', 'yuzu', 'nand', 'user', 'save'), 'yuzu'),
                         (('.config', 'Ryujinx', 'bis', 'user', 'save'), 'ryujinx')):
        os.makedirs(os.path.join(deck, *link[:-1]))
        os.symlink(os.path.join(real, target), os.path.join(deck, *link))
    check('save folders that are links (EmuDeck) are followed', [(c['source'], slot0(c['path'])) for c in
          ll.find_emulator_saves({'HOME': deck})] == [('yuzu', 'deck yuzu'), ('Ryujinx', 'deck ryujinx')])

    sd = os.path.join(t, 'sd', 'JKSV', 'The Legend of Zelda Tears of the Kingdom')
    fake_slot(os.path.join(sd, 'Player - 2026-10-01'), 0, 'jksv1', 100)
    fake_slot(os.path.join(sd, 'Player - 2026-10-20'), 0, 'jksv2', 50)
    j = ll.find_saves_under(os.path.join(t, 'sd'), 4)
    check('a JKSV folder: both backups, newest first', [os.path.basename(c['path']) for c in j] ==
          ['Player - 2026-10-20', 'Player - 2026-10-01'] and {c['source'] for c in j} == {'folder'})
    check('a JKSV folder: too shallow', ll.find_saves_under(os.path.join(t, 'sd'), 1) == [])
    check('a JKSV folder: 5 levels down by default', len(ll.find_saves_under(os.path.join(t, 'sd'))) == 2)

    # the menu's own order: the game folder's import=, our other game versions, then the emulators
    pkg = os.path.join(t, 'game', 'TotK-PC-x')
    fake_slot(os.path.join(t, 'pc-save'), 0, 'pc', 70)
    write(os.path.join(pkg, 'player.txt'), f'commit=abc\nimport={os.path.join(t, "pc-save")}\n')
    data = ll.user_dirs({'HOME': home})[0]
    fake_slot(os.path.join(data, 'userdata-v143', 'save'), 0, 'v143', 80)
    found = ll.find_saves(pkg, {'HOME': home})
    check('the menu: import=, then nxrecomp v143, then the emulators', [c['source'] for c in found] ==
          ['save on this PC', 'nxrecomp v143', 'Ryujinx', 'eden', 'citron', 'yuzu', 'Ryujinx', 'nxrecomp'],
          [c['source'] for c in found])
    return home, p, sd


def test_replace(t, home, p):
    data = ll.user_dirs({'HOME': home})[0]
    userdata = os.path.join(data, 'userdata')
    save = os.path.join(userdata, 'save')
    check('no save yet: nothing to back up', ll.backup_from(data, save, 'auto') == (0, None) and
          ll.backup_save(data, userdata, 'auto') is None)

    check('import yuzu', ll.replace_save(data, p['yuzu'], sv.Report()) == 0 and slot0(save) == 'yuzu')
    cap = os.path.join(save, 'slot_00', 'caption.sav')
    check('a read-only file from the source is writable in the copy', os.stat(cap).st_mode & stat.S_IWUSR != 0)
    for step in (1, 2, 3):   # a failure at each step keeps the yuzu save and leaves nothing behind
        ll.FAIL_STEP = step
        try:
            rc = ll.replace_save(data, p['eden'], sv.Report())
        finally:
            ll.FAIL_STEP = 0
        check(f'import failing at step {step}: the save as it was, nothing left', (rc, slot0(save),
              os.path.lexists(save + '.new'), os.path.lexists(save + '.old')) == (step, 'yuzu', False, False))
    check('a source that is not a save: refused', ll.replace_save(data, os.path.join(home, '.config'), sv.Report()) == 1
          and slot0(save) == 'yuzu')

    # an import cut between its renames: recovery puts the old save back; a leftover save.new goes
    os.rename(save, save + '.old')
    ll.copy_tree(p['eden'], save + '.new')
    ll.recover_save(data)
    check('an interrupted import repaired', (slot0(save), os.path.lexists(save + '.old'),
          os.path.lexists(save + '.new')) == ('yuzu', False, False))
    ll.copy_tree(p['eden'], save + '.old')
    ll.recover_save(data)
    check('a save.old beside a save goes', not os.path.lexists(save + '.old') and slot0(save) == 'yuzu')

    # backups: 1 manual + 13 auto, pruned to the newest 10 auto as they are made; names sort by age
    rc, manual = ll.backup_from(data, save, 'manual')
    check('a manual backup', rc == 1 and manual.endswith('-manual') and slot0(manual) == 'yuzu')
    last = [ll.backup_save(data, userdata, 'auto') for _ in range(13)][-1]
    names = sorted(os.listdir(os.path.join(data, 'backups')))
    check('backups: the newest 10 automatic ones and the manual one', (len(names), sum(n.endswith('-auto') for n in
          names), sum(n.endswith('-manual') for n in names)) == (11, 10, 1), names)
    check('backups: named save-YYYYMMDD-HHMMSS-NN-<why>, the last made sorts last',
          all(len(n.split('-')) == 5 and n.startswith('save-') for n in names) and last and
          names[-1] == os.path.basename(last))

    # restore a backup over the eden import
    check('restore a backup', ll.replace_save(data, p['eden'], sv.Report()) == 0 and slot0(save) == 'eden' and
          ll.replace_save(data, last, sv.Report()) == 0 and slot0(save) == 'yuzu')

    # links are not copied, and deleting a tree removes a link in it, not what it points at
    src, target, dst = (os.path.join(t, 'link', n) for n in ('src', 'target', 'dst'))
    fake_slot(src, 0, 'linked')
    fake_slot(target, 0, 'target')
    os.symlink(target, os.path.join(src, 'link'))
    os.symlink(os.path.join(target, 'slot_00', 'progress.sav'), os.path.join(src, 'file-link'))
    ll.copy_tree(src, dst)
    check('links are not copied', (slot0(dst), os.path.lexists(os.path.join(dst, 'link')),
          os.path.lexists(os.path.join(dst, 'file-link'))) == ('linked', False, False))
    check('deleting a tree: the link goes, not what it points at', ll.delete_tree(src) and not os.path.lexists(src) and
          slot0(target) == 'target')
    # a read-only folder (as copied from an SD card or a read-only medium) is deleted too
    os.chmod(os.path.join(dst, 'slot_00'), 0o555)
    os.chmod(dst, 0o555)
    check('a read-only folder is deleted', ll.delete_tree(dst) and not os.path.lexists(dst))

    # play() repairs an interrupted import before the game starts (launcher.c: saves_recover at start)
    os.rename(save, save + '.old')
    with home_env(home):
        run_package, incomplete = ll.run_package, ll.incomplete
        ll.run_package, ll.incomplete = (lambda pkg: 0), (lambda pkg: '')
        try:
            rc = ll.play(os.path.join(t, 'game', 'TotK-PC-x'))
        finally:
            ll.run_package, ll.incomplete, ll._LOG = run_package, incomplete, None
    check('play: an interrupted import repaired first', rc == 0 and slot0(save) == 'yuzu')


def test_import(t):
    """launcher.c replace_with + saves_replace_report with the converter (test_savever.c test_import): a refused save
    leaves the current one and the source as they were; files that are not game save files go in as they are; a later
    version's save is converted in the copy, with the original and the previous save in the backups"""
    home = os.path.join(t, 'home-import')
    env = {'HOME': home}
    data = ll.user_dirs(env)[0]
    save = os.path.join(data, 'userdata', 'save')
    write(os.path.join(save, 'slot_00', 'progress.sav'), 'current')
    src = os.path.join(t, 'later')
    bad = tsv.later_save(rev=sv.tables().latest)   # a real later revision, keys the real tables do not know
    write(os.path.join(src, 'slot_00', 'progress.sav'), bad)
    code, rep, bk, orig = ll.import_save(src, env)
    check('import refused: why', (code, rep.refused, rep.file) == (4, sv.SV_UNKNOWN_KEYS, 'slot_00/progress.sav'),
          (code, rep))
    check('import refused: the save, the source and the folder as they were', slot0(save) == 'current' and
          open(os.path.join(src, 'slot_00', 'progress.sav'), 'rb').read() == bad and
          not os.path.lexists(save + '.new'))
    check('import refused: the backups made first (as launcher.c)', bool(bk) and bk.endswith('-before-import') and
          slot0(bk) == 'current' and bool(orig) and orig.endswith('-original'))
    text = ll.import_text(code, rep, bk, orig, src)
    check('import refused: said', text.startswith("The save wasn't imported; your previous save is unchanged.") and
          'slot_00/progress.sav has data that version 1.0.0' in text, text)
    write(os.path.join(src, 'slot_00', 'progress.sav'), 'not a game save file')
    code, rep, bk, orig = ll.import_save(src, env)
    check('files that are not game save files go in as they are', (code, rep.files, rep.refused, orig,
          slot0(save)) == (0, 0, 0, None, 'not a game save file'), (code, rep, orig))
    check('every import backs up first', len(glob.glob(os.path.join(data, 'backups', 'save-*-before-import'))) == 2)

    # a later version's save end to end, on test_savever's synthetic tables
    later = tsv.later_save()
    write(os.path.join(src, 'slot_00', 'progress.sav'), later)
    sv._T = tsv.T
    try:
        code, rep, bk, orig = ll.import_save(src, env)
        text = ll.import_text(code, rep, bk, orig, src)
    finally:
        sv._T = None
    with open(os.path.join(save, 'slot_00', 'progress.sav'), 'rb') as f:
        got = f.read()
    check('a later save: converted on the way in', (code, rep.files, rep.from_rev, len(got), sv._rd32(got, 4)) ==
          (0, 1, tsv.REV_NEW, tsv.FSIZE, tsv.REV_OLD), (code, rep))
    check('a later save: the original, unconverted, and the previous save in the backups; the source untouched',
          bool(orig) and open(os.path.join(orig, 'slot_00', 'progress.sav'), 'rb').read() == later and
          slot0(bk) == 'not a game save file' and
          open(os.path.join(src, 'slot_00', 'progress.sav'), 'rb').read() == later)
    before = slot0(save)
    load = sv.load_tables
    sv._T, sv.load_tables = None, (lambda path=None: open(os.path.join(t, 'no-such-header.h')))
    try:   # (its own names: text, bk and orig above are checked below)
        missing = ll.import_save(src, env)
    finally:
        sv._T, sv.load_tables = None, load
    check('the converter\'s tables missing: nothing done, and what to do', (missing[0], missing[2], missing[3],
          slot0(save)) == (7, None, None, before) and 'press Install' in ll.import_text(*missing, src), missing)
    texts = {sv.SV_UNKNOWN_VERSION: "slot_00/progress.sav comes from a game version the converter doesn't know "
                                    "(revision 7).",
             sv.SV_UNKNOWN_KEYS: "slot_00/progress.sav has data that version 1.0.0 doesn't have",
             sv.SV_TOO_BIG: 'slot_00/progress.sav holds more than a 1.0.0 save has room for.',
             sv.SV_PICTURE: "The picture in slot_00/progress.sav couldn't be converted.",
             sv.SV_IO: "slot_00/progress.sav couldn't be read or written.",
             sv.SV_BAD_FILE: 'slot_00/progress.sav is damaged or incomplete.'}
    said = {k: ll.refusal_text(sv.Report(refused=k, detail=7, file='slot_00/progress.sav')) for k in texts}
    check('every refusal in words (launcher.c refusal_text)', all(said[k].startswith(v) for k, v in texts.items()), said)
    check('a later save: said as on Windows', text.startswith('Done.\n\nThe save was from version 1.4.x and is now a '
          '1.0.0 save:') and "- 4 entries from later updates that 1.0.0 doesn't have were left out." in text and
          f'The original save, not converted, is kept in:\n{orig}' in text and
          f'Your previous save is backed up in:\n{bk}' in text, text)
    rep = sv.Report(refused=sv.SV_PICTURE, file='slot_00/caption.sav')
    real = sv._pil
    sv._pil = lambda: None
    try:
        check('a picture refused without Pillow: what to do', 'Saves button in the Setup' in ll.refusal_text(rep))
    finally:
        sv._pil = real


def test_cli(t, home, p, sd):
    """--saves / --import-save, the flags anywhere ('TotK Launcher' puts its folder first)"""
    pkg = os.path.join(t, 'game', 'TotK-PC-x')
    with home_env(home):
        data, cache = ll.user_dirs()
        eden_users = os.path.dirname(os.path.dirname(p['eden']))
        for user, rev, minutes in (('G' * 32, sv.REV_11X, 9), ('H' * 32, 12345, 8)):   # (only the revision word)
            write(os.path.join(eden_users, user, TOTK, 'slot_00', 'progress.sav'),
                  sv.MAGIC.to_bytes(4, 'little') + rev.to_bytes(4, 'little'), minutes)
        rc, out = run_main('--saves', '--json')
        s = json.loads(out) if rc == 0 else {}
        check('--saves --json: the current save and the saves found', rc == 0 and s['save'] ==
              os.path.join(data, 'userdata', 'save') and s['slots'] == 1 and
              [c['source'] for c in s['saves']][:3] == ['nxrecomp v143', 'Ryujinx', 'eden'], out)
        users = {c['user']: c for c in s.get('saves', ()) if c['source'] == 'eden'}
        check('--saves --json: each save\'s version, slots and time', [(users.get(u, {}).get('version'),
              users.get(u, {}).get('slots'), len(users.get(u, {}).get('saved', ''))) for u in ('G' * 32, 'H' * 32,
              'B' * 32)] == [('1.1.x / 1.2.x', 1, 16), ('unknown version', 1, 16), ('', 2, 16)], users)
        rc, out = run_main(pkg, '--saves')
        check('--saves: in words, with the game folder\'s import=', rc == 0 and 'Saves you can import' in out and
              'save on this PC' in out and '--import-save' in out, out)
        rc, out = run_main('--saves', '--look-in', os.path.join(t, 'sd'), '--json')
        check('--saves --look-in: the folder\'s saves only', rc == 0 and
              [c['source'] for c in json.loads(out)['saves']] == ['folder', 'folder'], out)
        rc, out = run_main(pkg, '--look-in', os.path.join(t, 'sd'), '--json')
        check('--look-in alone means --saves, never play', rc == 0 and len(json.loads(out)['saves']) == 2, out)
        rc, out = run_main(pkg, '--json')
        check('--json alone means --saves', rc == 0 and 'saves' in json.loads(out), out)
        jksv2 = os.path.join(sd, 'Player - 2026-10-20')
        rc, out = run_main(pkg, '--import-save', jksv2)
        check('--import-save: done, in words', (rc, out.startswith('Done.'),
              slot0(os.path.join(data, 'userdata', 'save'))) == (0, True, 'jksv2'), out)
        rc, out = run_main('--import-save', os.path.join(t, 'sd'))
        check('--import-save of a folder that holds saves: they are listed, nothing imported', rc == 2 and
              jksv2 in out and slot0(os.path.join(data, 'userdata', 'save')) == 'jksv2', out)
        lock = ll.game_lock(cache)
        try:
            rc, out = run_main('--import-save', os.path.join(sd, 'Player - 2026-10-01'))
        finally:
            lock.close()
        check('--import-save while the game runs: refused', rc == 1 and 'The game is running' in out and
              slot0(os.path.join(data, 'userdata', 'save')) == 'jksv2', out)
        check('--import-save without a folder: usage', run_main('--import-save')[0] == 2)
        with open(os.path.join(cache, 'launcher.log'), encoding='utf-8') as f:
            check('the import is in launcher.log', 'import from ' + jksv2 in f.read())


def test_package(t, home, p):
    """package.py's Linux folder carries the converter and its tables, and its own launcher imports with them, run as
    'TotK Launcher' runs it (a process of its own, the folder first): the savever_data.h it reads is the one beside
    it"""
    sys.path.insert(0, os.path.join(REPO, 'engine', 'launcher', 'win'))
    import package  # noqa: E402
    inst = os.path.join(t, 'inst')
    game = os.path.join(inst, 'game', 'TotK-PC-1')
    deps = os.path.join(t, 'deps')
    write(os.path.join(deps, 'sdl3-install', 'lib', 'libSDL3.so.0'), b'\x7fELF')
    os.makedirs(game)
    saved = os.environ.get('NXR_LINUX_DEPS')
    os.environ['NXR_LINUX_DEPS'] = deps
    try:
        package.linux_launcher(game)
    finally:
        if saved is None:
            del os.environ['NXR_LINUX_DEPS']
        else:
            os.environ['NXR_LINUX_DEPS'] = saved
    check('package.py: the Linux folder has the launcher, the converter and its tables', all(os.path.isfile(
          os.path.join(game, f)) for f in ('launcher_linux.py', 'savever.py', 'savever_data.h', 'TotK Launcher')))
    write(os.path.join(game, 'totk'), b'')
    write(os.path.join(game, 'player.txt'), 'commit=abc\n')
    with home_env(home):
        save = os.path.join(ll.user_dirs()[0], 'userdata', 'save')
        r = subprocess.run([sys.executable, os.path.join(game, 'launcher_linux.py'), game, '--import-save',
                            p['nxrecomp']], capture_output=True, text=True, stdin=subprocess.DEVNULL)
    check('the game folder\'s own launcher imports', (r.returncode, r.stdout.startswith('Done.'), slot0(save)) ==
          (0, True, 'nxrecomp'), r.stdout + r.stderr)
    return inst


def test_window(t, home, p, inst):
    """the AppImage window's Saves helpers, through the game folder's launcher (run as Play runs it: there is no
    <install>/tools/python here, so by this Python)"""
    app = os.path.join(t, 'app')
    os.makedirs(app)
    shutil.copy(os.path.join(REPO, 'tools', 'linux', 'appimage', 'setup_gui.py'), app)
    write(os.path.join(app, 'release.json'), '{"tag": "linux-test-x", "repo": "o/r", "commit": "' + '0' * 40 + '"}')
    spec = importlib.util.spec_from_file_location('setup_gui_saves', os.path.join(app, 'setup_gui.py'))
    sg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sg)
    try:
        sg.list_saves(os.path.join(t, 'no-install'))
        check('window: no game yet', False)
    except RuntimeError as e:
        check('window: no game yet', 'install it first' in str(e), e)
    with home_env(home):
        save = os.path.join(ll.user_dirs()[0], 'userdata', 'save')
        check('window: the data folder and the save are the launcher\'s (Play\'s offer looks there first)',
              (sg.player_data(), sg.has_save(sg.player_data()), sg.has_save(os.path.join(t, 'none'))) ==
              (ll.user_dirs()[0], True, False))
        found = sg.list_saves(inst)
        check('window: the current save and the saves found', (found['save'], found['slots'], 'eden' in
              [c['source'] for c in found['saves']]) == (save, 1, True), found)
        found = sg.list_saves(inst, os.path.join(t, 'sd'))
        check('window: Look in another folder', [c['source'] for c in found['saves']] == ['folder', 'folder'], found)
        rc, text = sg.import_save(inst, p['eden'])
        check('window: Import the selected save', (rc, text.startswith('Done.'), slot0(save)) == (0, True, 'eden'),
              (rc, text))
        old = os.path.join(inst, 'game', 'TotK-PC-2')   # a newer folder made before the import existed
        write(os.path.join(old, 'totk'), b'')
        write(os.path.join(old, 'player.txt'), 'commit=def\n')
        shutil.copy(os.path.join(inst, 'game', 'TotK-PC-1', 'launcher_linux.py'), old)
        future = time.time() + 60
        os.utime(old, (future, future))
        try:
            sg.import_save(inst, p['yuzu'])
            check('window: a game folder from before the import is not asked', False)
        except RuntimeError as e:
            check('window: a game folder from before the import is not asked', 'press Install' in str(e) and
                  slot0(save) == 'eden', e)


def main():
    t = tempfile.mkdtemp(prefix='nxr_test_saves_')
    try:
        home, p, sd = test_find(t)
        test_replace(t, home, p)
        test_import(t)
        test_cli(t, home, p, sd)
        inst = test_package(t, home, p)
        test_window(t, home, p, inst)
    finally:
        check('the test folder deleted (read-only files in it too)', ll.delete_tree(t))
    print(f'test_saves: {len(fails)} FAILED' if fails else 'test_saves: all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
