"""Setup's languages (Builder.cs class L): every sentence the window shows is L.S("<English>") and each
tools/builder/gui/text/<code>.json must translate exactly those (none missing, none stale), keep each one's {0} {1}
values and its leading / trailing spaces; then Builder.cs is compiled with the files built in, as make_setup.py does
(Windows' own C# compiler, no install), and test_text.cs checks the exe reads every file as Python does, formats every
sentence, and picks the language from --lang and the display language.
Run: python tools/builder/gui/test_text.py   (exit 1 on any failure)
     python tools/builder/gui/test_text.py --keys   the English sentences as a JSON object (a new language's start)"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT = os.path.join(HERE, 'text')
sys.path.insert(0, HERE)
from make_setup import CSC, text_resources  # noqa: E402

ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'", '0': '\0'}


def literal(src, i):
    """The C# string literal at src[i] ("..." or @"..."): its value and the index after it."""
    if src[i] == '@':
        j, out = i + 2, []
        while True:
            if src[j] == '"':
                if src[j + 1:j + 2] == '"':
                    out.append('"')
                    j += 2
                    continue
                return ''.join(out), j + 1
            out.append(src[j])
            j += 1
    j, out = i + 1, []
    while src[j] != '"':
        if src[j] == '\\':
            c = src[j + 1]
            if c == 'u':
                out.append(chr(int(src[j + 2:j + 6], 16)))
                j += 6
                continue
            out.append(ESC[c])
            j += 2
            continue
        out.append(src[j])
        j += 1
    return ''.join(out), j + 1


def concat(src, i):
    """"a" + "b" + ... from src[i] (whitespace and newlines between): the value, or None when it is not only literals."""
    parts = []
    while True:
        while src[i].isspace():
            i += 1
        if src[i] != '"' and src[i:i + 2] != '@"':
            return None
        v, i = literal(src, i)
        parts.append(v)
        while src[i].isspace():
            i += 1
        if src[i] != '+':
            return ''.join(parts)
        i += 1


def keys(src):
    """Every L.S sentence of Builder.cs: a literal (or literals joined by +) or a const string of them."""
    consts = {}
    for m in re.finditer(r'const string (\w+) = ', src):
        v = concat(src, m.end())
        if v is not None:
            consts[m.group(1)] = v
    found, bad = set(), []
    for m in re.finditer(r'\bL\.S\(', src):
        line = src.count('\n', 0, m.start()) + 1
        if src[:m.start()].rsplit('\n', 1)[-1].lstrip().startswith('//'):
            continue
        v = concat(src, m.end())
        if v is None:
            name = re.match(r'\s*(\w+)\s*[,)]', src[m.end():])
            v = consts.get(name.group(1)) if name else None
        if v is None:
            bad.append(line)
        else:
            found.add(v)
    return found, bad


def holes(s):
    """The {n} / {n:fmt} values of a format string, sorted; None when its braces are not a valid .NET format."""
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '{':
            if s[i + 1:i + 2] == '{':
                i += 2
                continue
            j = s.find('}', i)
            m = re.fullmatch(r'(\d+)(,-?\d+)?(:[^{}]*)?', s[i + 1:j]) if j > 0 else None
            if not m:
                return None
            out.append(s[i + 1:j])
            i = j + 1
        elif c == '}':
            if s[i + 1:i + 2] != '}':
                return None
            i += 2
        else:
            i += 1
    return sorted(out)


def check_files(english):
    fails = 0
    codes = re.search(r'Codes = \{([^}]*)\}', open(os.path.join(HERE, 'Builder.cs'), encoding='utf-8').read()).group(1)
    codes = re.findall(r'"([^"]+)"', codes)
    files = sorted(f[:-5] for f in os.listdir(TEXT) if f.endswith('.json'))
    if sorted(codes) != files:
        print(f'FAIL L.Codes {codes} != text/*.json {files}')
        fails += 1
    for code in files:
        with open(os.path.join(TEXT, code + '.json'), encoding='utf-8') as f:
            tr = json.load(f)
        missing, stale = english - tr.keys(), tr.keys() - english
        for k in sorted(missing):
            print(f'FAIL {code}: missing {k[:100]!r}')
        for k in sorted(stale):
            print(f'FAIL {code}: stale {k[:100]!r}')
        fails += len(missing) + len(stale)
        for k, v in tr.items():
            why = None
            if not isinstance(v, str) or not v.strip():
                why = 'empty'
            elif holes(v) is None:
                why = 'braces are not a .NET format'
            elif k in english and (holes(k) if '{' in k else []) != holes(v):
                why = f'values {holes(k)} != {holes(v)}'
            elif k[:1].isspace() != v[:1].isspace() or k[-1:].isspace() != v[-1:].isspace():
                why = 'leading / trailing space differs'
            elif k.count('\n') != v.count('\n'):
                why = 'line breaks differ'
            if why:
                print(f'FAIL {code}: {why}: {k[:80]!r} -> {v[:80]!r}')
                fails += 1
        print(f'{code}: {len(tr)} sentences')
    return fails, files


def check_exe(files, english):
    with tempfile.TemporaryDirectory(prefix='settext') as tmp:
        rel, exe = os.path.join(tmp, 'Release.cs'), os.path.join(tmp, 'test_text.exe')
        with open(rel, 'w', encoding='utf-8') as f:
            f.write('static class Release { public const string Product = "Test Product", Tag = "v0.0.0", Repo = "x/y", '
                    'LlvmVersion = "1", Commit = "", LlvmSha256 = "", LlvmAsset = "", LlvmTag = "", VulkanSha256 = "", VulkanAsset = "", '
                    'VulkanTag = ""; }\n')
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:TextTest', f'/out:{exe}',
                            '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.IO.Compression.dll',
                            '/r:System.IO.Compression.FileSystem.dll', os.path.join(HERE, 'Builder.cs'), rel,
                            os.path.join(HERE, 'test_text.cs')] + text_resources(), capture_output=True, text=True)
        if r.returncode:
            print('FAIL csc:\n' + r.stdout + r.stderr)
            return 1
        out_file = os.path.join(tmp, 'out.json')
        p = subprocess.run([exe, out_file] + files, capture_output=True, text=True, timeout=60)
        sys.stdout.write(p.stdout + p.stderr)
        fails = p.returncode
        with open(out_file, encoding='utf-8') as f:
            got = json.load(f)
        for code in files:
            with open(os.path.join(TEXT, code + '.json'), encoding='utf-8') as f:
                want = json.load(f)
            if got.get(code) != want:
                print(f'FAIL {code}: the exe reads another text than json.load')
                fails += 1
        return fails


def main():
    sys.stdout.reconfigure(errors='backslashreplace')   # (a console without Cyrillic or CJK still gets the failures)
    src = open(os.path.join(HERE, 'Builder.cs'), encoding='utf-8').read()
    english, bad = keys(src)
    if '--keys' in sys.argv:
        print(json.dumps({k: k for k in sorted(english)}, ensure_ascii=False, indent=1))
        return 0
    fails = len(bad)
    for line in bad:
        print(f'FAIL Builder.cs:{line}: L.S takes a literal (or a const string of them), else no file can translate it')
    print(f'Builder.cs: {len(english)} sentences')
    f, files = check_files(english)
    fails += f
    fails += check_exe(files, english)
    print('all passed' if not fails else f'FAILED ({fails})')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
