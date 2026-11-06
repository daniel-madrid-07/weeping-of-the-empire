"""builder.main()'s folder names are set once and never reused for something else (from the code, a second):

- `out` (the game folder step 8 packages into) is assigned exactly once in main(): v0.1.9-v0.1.9.2's firmware-fonts
  step (`ok, out = run(...)`, #26's f0e4bd69a) overwrote it with that step's text output, and step 8 failed whenever
  that step ran (every run when the firmware's fonts fail, the first run when they don't);
- the same for `work`, `install`, `tools`, `vdir` and `v100`, the other folders the steps after it read.

    python -I tools/builder/test_main_names.py
"""
import ast, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
tree = ast.parse(open(os.path.join(HERE, 'builder.py'), encoding='utf-8').read())
main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main')
fails = []


def targets(node):
    """the plain names a statement binds (assignments, tuple unpacking, for loops, with ... as, walrus)"""
    out = []

    def names(t):
        if isinstance(t, ast.Name):
            out.append(t.id)
        elif isinstance(t, (ast.Tuple, ast.List)):
            for e in t.elts:
                names(e)
        elif isinstance(t, ast.Starred):
            names(t.value)
    if isinstance(node, ast.Assign):
        for t in node.targets:
            names(t)
    elif isinstance(node, (ast.AugAssign, ast.AnnAssign, ast.For, ast.comprehension)):
        names(node.target)
    elif isinstance(node, ast.withitem) and node.optional_vars is not None:
        names(node.optional_vars)
    elif isinstance(node, ast.NamedExpr):
        names(node.target)
    return out


bound = {}
for node in ast.walk(main):
    if isinstance(node, (ast.FunctionDef, ast.Lambda)) and node is not main:
        continue   # (a nested def / lambda has its own names; ast.walk still visits its body: rare here, and harmless)
    for name in targets(node):
        bound.setdefault(name, []).append(getattr(node, 'lineno', 0))
for name in ('out', 'work', 'install', 'tools', 'vdir', 'v100'):
    lines = bound.get(name, [])
    ok = len(lines) == 1
    print(f"{'PASS' if ok else 'FAIL'} main() sets `{name}` once {lines}", flush=True)
    if not ok:
        fails.append(name)
print('ALL PASS' if not fails else f'{len(fails)} FAIL: {fails}')
sys.exit(1 if fails else 0)
