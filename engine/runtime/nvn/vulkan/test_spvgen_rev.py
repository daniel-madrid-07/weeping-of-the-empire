"""Keeps cache/spirv honest (shaders.c src_hash folds SPVGEN_REV in): fails when spvgen.c or spvgen.h (which spvgen.c
includes; without its own hash line) no longer match the sha256 recorded next to SPVGEN_REV in spvgen.h. Then: if
spvgen's SPIR-V for the same GLSL changed, bump SPVGEN_REV (players' cached modules from the old spvgen are then not
served); record the new hash either way.
Usage: python test_spvgen_rev.py"""
import hashlib, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HASH_LINE = re.compile(r'// spvgen\.c \+ spvgen\.h sha256: (\S+)\n')
h = open(os.path.join(HERE, 'spvgen.h'), 'rb').read().replace(b'\r\n', b'\n').decode('utf-8')
src = open(os.path.join(HERE, 'spvgen.c'), 'rb').read().replace(b'\r\n', b'\n') + HASH_LINE.sub('', h).encode('utf-8')
want = HASH_LINE.search(h)
rev = re.search(r'#define SPVGEN_REV (\d+)', h)
got = hashlib.sha256(src).hexdigest()
if not want or not rev:
    print('FAIL: spvgen.h lacks SPVGEN_REV or the recorded spvgen.c + spvgen.h sha256')
    sys.exit(1)
if want[1] != got:
    print(f'FAIL: spvgen.c / spvgen.h changed (sha256 {got}, recorded {want[1]}): bump SPVGEN_REV (now {rev[1]}) if its '
          'SPIR-V for the same GLSL changed, and record the new hash in spvgen.h')
    sys.exit(1)
print(f'ok: spvgen.c + spvgen.h match SPVGEN_REV {rev[1]}')
