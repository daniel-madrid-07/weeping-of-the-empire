"""builder.check_dump: what would stop step 1 is said before it, in one plain sentence with the fix (a wrong file, keys
that cannot open it, a personalized ticket, a short copy, an update that is not one). Synthetic containers
(pipeline/tests/synthetic.py: encrypted NCAs, no game data, no real keys); seconds.

    <venv python> -I tools/builder/test_dump_check.py
"""
import os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [HERE, REPO]
import builder  # noqa: E402
from pipeline.tests import synthetic as syn  # noqa: E402

fails = []
tmp = tempfile.mkdtemp(prefix='dumpcheck-')


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


def put(name, data):
    p = os.path.join(tmp, name)
    with open(p, 'wb' if isinstance(data, (bytes, bytearray)) else 'w') as f:
        f.write(data)
    return p


keys = put('prod.keys', syn.keys_text())
no_area = put('no-area.keys', ''.join(ln + '\n' for ln in syn.keys_text().splitlines() if 'key_area' not in ln))
no_header = put('no-header.keys', ''.join(ln + '\n' for ln in syn.keys_text().splitlines() if 'header_key' not in ln))
nsp = put('game.nsp', syn.game_image('nsp'))
secrets = [k.hex() for k in syn.SECRETS]


def said(name, msg, *want):
    ok = all(w in msg for w in want) and not any(s in msg.lower() for s in secrets)
    check(name, ok, repr(msg[:160]))


check('a good nsp opens', builder.check_dump(nsp, keys) == '')
check('a good xci opens', builder.check_dump(put('game.xci', syn.game_image('xci')), keys) == '')
check('a ticketed (common) nsp opens', builder.check_dump(put('managed.nsp', syn.game_image('nsp', managed=True)), keys) == '')
said('keys without the key area', builder.check_dump(nsp, no_area), 'Your prod.keys cannot open the dump', 'key_area_key',
     'dump prod.keys again')
said('keys without header_key', builder.check_dump(nsp, no_header), 'Your prod.keys cannot open the dump')
said('not a game file', builder.check_dump(put('photo.nsp', os.urandom(8192)), keys), 'is not a complete .nsp / .xci')
said('another game', builder.check_dump(put('other.nsp', syn.game_image('nsp', title=0x0100000000001000)), keys),
     "is not Tears of the Kingdom's base game", 'Update row')
said('a personalized ticket', builder.check_dump(put('pers.nsp', syn.partition(
    [('program.nca', syn.nca(True)), ('t.tik', syn.ticket(personalized=True))])), keys), 'personalized ticket')
full = syn.game_image('nsp')
said('a short copy', builder.check_dump(put('short.nsp', full[:len(full) // 3]), keys), 'not a complete')
said('the base game as the update', builder.check_dump(nsp, keys, nsp), 'The update dump is not')
said('a keys file that is not one', builder.check_dump(nsp, put('bad.keys', 'header_key = zz\n')), 'cannot be read as a keys file')
check('a missing file', 'cannot be read' in builder.check_dump(os.path.join(tmp, 'gone.nsp'), keys))

# tickets: a rights-managed NCA without its ticket, an unknown ticket type, two that disagree
managed = [('program.nca', syn.nca(True)), ('control.nca', syn.nca(True, content=2))]
said('no ticket', builder.check_dump(put('noticket.nsp', syn.partition(managed)), keys), 'has no usable ticket',
     'common ticket')
odd = bytearray(syn.ticket())
odd[0:4] = (0x12345).to_bytes(4, 'little')
said('a ticket of an unknown type', builder.check_dump(put('oddtik.nsp', syn.partition(
    managed + [('t.tik', bytes(odd))])), keys), 'has no usable ticket')
said('two tickets that disagree', builder.check_dump(put('twotik.nsp', syn.partition(
    managed + [('a.tik', syn.ticket()), ('b.tik', syn.ticket(bad_key=True))])), keys), 'has no usable ticket')
# headers decrypted to nonsense (other keys, or damage): the pipeline's exact words through the mapping
for msg in ('FS header hash mismatch', 'Unsupported NCA key area'):
    said(f'"{msg}"', builder.dump_problem('the dump', msg), 'could not be read correctly', 'dump prod.keys again', 'damaged')

print('all passed' if not fails else f'FAILED: {fails}')
sys.exit(1 if fails else 0)
