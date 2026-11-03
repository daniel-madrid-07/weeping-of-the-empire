"""The Linux AppImage's window: Setup and Play (linux-port, 2026-11-02). The Windows Setup's job (tools/builder/gui/
Builder.cs) on Linux, small: the player picks their own dump, keys file (and optional firmware / 1.4.3 update) and a
folder; Install fetches this release's source and runs tools/builder/builder.py --install there, showing its lines and
progress; Play starts the newest game folder through its launcher (engine/launcher/linux/launcher_linux.py).

    WeepingOfTheEmpire-x86_64.AppImage                          the window
    WeepingOfTheEmpire-x86_64.AppImage --play                   play at once, no window (Steam / Gaming Mode)
    WeepingOfTheEmpire-x86_64.AppImage --setup <dump> <keys> [<firmware>] [--update <nsp>] [--install DIR]   no window

Install layout (as Setup's on Windows): <install>/src (the public repository at this AppImage's tag, git), <install>/tools
(this AppImage's Python copied there, the compiler and the Linux SDK: toolchain.py), <install>/work, <install>/game.
Remembered in $XDG_CONFIG_HOME/weeping-of-the-empire/setup.json: the install folder and the files' paths, never a key.
Setup's lines also go to <install>/setup.log (the one before: setup.prev.log).
"""
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
APPDIR = os.environ.get('APPDIR') or os.path.dirname(os.path.dirname(HERE))
PYHOME = os.path.join(APPDIR, 'usr', 'python')
with open(os.path.join(HERE, 'release.json'), encoding='utf-8') as f:
    RELEASE = json.load(f)  # {"tag": "v0.1.9", "repo", "commit": "<40 hex>"[, "bundle": true]} (make_appimage.sh)
# A test build carries what a release downloads (make_appimage.sh NXR_BUNDLE_DIR): the source as a git bundle (its tag
# inside) and the two Linux toolchain archives, for a version that has no public tag or release assets yet.
BUNDLE = os.path.join(APPDIR, 'usr', 'bundle') if RELEASE.get('bundle') else None
CONFIG = os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.join(os.path.expanduser('~'), '.config'),
                      'weeping-of-the-empire', 'setup.json')
DEFAULT_INSTALL = os.path.join(os.path.expanduser('~'), 'Games', 'WeepingOfTheEmpire')
PERCENT = re.compile(r'(\d{1,3})% \(')
STEP = re.compile(r'step (\d+)/(\d+)')


def load_config():
    try:
        with open(CONFIG, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(c):
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    with open(CONFIG + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(c, f, indent=1)
    os.replace(CONFIG + '.tmp', CONFIG)


def newest_game(install):
    g = os.path.join(install, 'game')
    folders = [os.path.join(g, d) for d in os.listdir(g)] if os.path.isdir(g) else []
    folders = [d for d in folders if os.path.isfile(os.path.join(d, 'totk')) and os.path.isfile(os.path.join(d, 'player.txt'))]
    return max(folders, key=os.path.getmtime) if folders else None


def tools_python(install, say):
    """<install>/tools/python: this AppImage's Python copied out once per AppImage version (the AppImage itself is
    read-only, and the build installs its packages into the Python it runs on); its interpreter"""
    dst = os.path.join(install, 'tools', 'python')
    stamp = os.path.join(dst, '.appimage-release')
    if not (os.path.isfile(stamp) and open(stamp).read().strip() == RELEASE['tag']):
        say(f'copying Python into {dst}')
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(PYHOME, dst, symlinks=True)
        with open(stamp, 'w') as f:
            f.write(RELEASE['tag'] + '\n')
    return os.path.join(dst, 'bin', 'python3')


def run(cmd, say, cwd=None, env=None):
    """cmd to its end, every line said; its exit code"""
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         text=True, encoding='utf-8', errors='replace', bufsize=1,
                         start_new_session=True)   # (its own process group: builder.py --gui-pid ends that group alone)
    for ln in p.stdout:
        for part in ln.rstrip('\n').split('\r'):
            if part.strip():
                say(part)
    return p.wait()


def source(install, say):
    """<install>/src at this AppImage's tag (only its own files: --depth 1), as Setup's Source on Windows"""
    git = shutil.which('git')
    if not git:
        raise RuntimeError('git is needed to fetch the source: install your distribution\'s git package, then press Install again')
    src = os.path.join(install, 'src')
    if not os.path.isdir(os.path.join(src, '.git')):
        shutil.rmtree(src, ignore_errors=True)
        if run([git, 'init', '--quiet', src], say):
            raise RuntimeError('git init failed')
    url = os.environ.get('NXR_SOURCE') or (os.path.join(BUNDLE, 'src.bundle') if BUNDLE else
                                           f'https://github.com/{RELEASE["repo"]}.git')
    say(f'fetching the source ({RELEASE["tag"]}{", from this AppImage" if url.endswith(".bundle") else ""})')
    depth = [] if url.endswith('.bundle') else ['--depth', '1']   # (a bundle: one exported commit, and no shallow fetch)
    if run([git, '-C', src, 'fetch', '--progress', *depth, '--force', url, RELEASE['tag']], say):
        raise RuntimeError(f'could not download the source ({RELEASE["tag"]}) from {url}: check the internet connection, '
                           'then press Install again')
    # only the commit this AppImage was made for (release.json "commit", as the Windows Setup's Release.Commit): a tag
    # moved on the repository, or NXR_SOURCE pointing elsewhere, is refused, not built
    got = subprocess.run([git, '-C', src, 'rev-parse', 'FETCH_HEAD^{commit}'], capture_output=True, text=True).stdout.strip()
    want = RELEASE.get('commit', '').lower()
    if want and got.lower() != want:
        raise RuntimeError(f'the source fetched for {RELEASE["tag"]} is commit {got or "?"}, not {want}, the one this '
                           f'Setup was made for: download this version\'s AppImage again')
    if run([git, '-C', src, 'checkout', '--quiet', '--force', '--detach', 'FETCH_HEAD'], say):
        raise RuntimeError('git checkout failed')
    return src


def install_game(install, inputs, update, say, gui_pid=None):
    """the whole Setup: Python out, the source, builder.py --install; its exit code. Every line also goes to
    <install>/setup.log (the one before: setup.prev.log), the file a player sends when something goes wrong."""
    os.makedirs(install, exist_ok=True)
    logp = os.path.join(install, 'setup.log')
    if os.path.isfile(logp):
        os.replace(logp, os.path.join(install, 'setup.prev.log'))
    with open(logp, 'w', encoding='utf-8', buffering=1) as logf:
        def tee(msg):
            logf.write(msg + '\n')
            say(msg)
        tee(f'Weeping of the Empire {RELEASE["tag"]} Setup (Linux AppImage{", test build" if BUNDLE else ""})')
        try:
            rc = _install(install, inputs, update, tee, gui_pid)
        except Exception as e:
            logf.write(f'ERROR: {e}\n')
            raise
        logf.write(f'exit {rc}\n')
        return rc


def _install(install, inputs, update, say, gui_pid):
    py = tools_python(install, say)
    src = source(install, say)
    cmd = [py, '-E', '-s', os.path.join(src, 'tools', 'builder', 'builder.py'), *inputs, '--install', install]
    if update:
        cmd += ['--update', update]
    if gui_pid:
        cmd += ['--gui-pid', str(gui_pid)]
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'NXR', 'TOTK_', 'LARECOMP_'))}
    if BUNDLE:   # (toolchain.py's local-archive inputs: a version without published Linux assets)
        env['NXR_LINUX_LLVM_ARCHIVE'] = os.path.join(BUNDLE, 'setup-linux-llvm.tar.zst')
        env['NXR_LINUX_SDK_ARCHIVE'] = os.path.join(BUNDLE, 'setup-linux-sdk.tar.zst')
    return run(cmd, say, cwd=src, env=env)


def play(install, say=print):
    game = newest_game(install)
    if not game:
        say(f'no game in {install}/game yet: install it first')
        return 2
    py = os.path.join(install, 'tools', 'python', 'bin', 'python3')
    return subprocess.call([py if os.path.isfile(py) else sys.executable, os.path.join(game, 'launcher_linux.py'), game])


# ---- the window ----------------------------------------------------------------------------------------------------

def window():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    cfg = load_config()
    root = tk.Tk()
    root.title(f'Weeping of the Empire {RELEASE["tag"]} - Setup{" (test build)" if BUNDLE else ""}')
    root.minsize(760, 520)
    v = {k: tk.StringVar(value=cfg.get(k, '')) for k in ('dump', 'keys', 'firmware', 'update')}
    v['install'] = tk.StringVar(value=cfg.get('install') or DEFAULT_INSTALL)
    lines, busy = queue.Queue(), {'on': False}
    frm = ttk.Frame(root, padding=10)
    frm.pack(fill='both', expand=True)
    ttk.Label(frm, text='Your own dump of Tears of the Kingdom 1.0.0 and your console\'s keys file. Nothing of the game '
                        'is downloaded: everything is built on this PC from your files.', wraplength=720).grid(
        row=0, column=0, columnspan=3, sticky='w', pady=(0, 8))

    def pick(key, kind):
        if kind == 'dir':
            p = filedialog.askdirectory(initialdir=v[key].get() or os.path.expanduser('~'))
        else:
            p = filedialog.askopenfilename(initialdir=os.path.dirname(v[key].get()) or os.path.expanduser('~'),
                                           filetypes=kind)
        if p:
            v[key].set(p)
    rows = [('dump', 'Game dump (.nsp / .xci / .nsz / .xcz)', [('Game dump', '*.nsp *.xci *.nsz *.xcz'), ('All', '*')]),
            ('keys', 'Keys file (prod.keys)', [('Keys', '*.keys'), ('All', '*')]),
            ('firmware', 'Firmware, optional (.zip; Korean / Chinese fonts)', [('Firmware', '*.zip'), ('All', '*')]),
            ('update', 'Update 1.4.3, optional (.nsp)', [('Update', '*.nsp *.xci'), ('All', '*')]),
            ('install', 'Install folder (about 25 GB)', 'dir')]
    for i, (key, label, kind) in enumerate(rows, start=1):
        ttk.Label(frm, text=label).grid(row=i, column=0, sticky='w')
        ttk.Entry(frm, textvariable=v[key], width=70).grid(row=i, column=1, sticky='ew', padx=6)
        ttk.Button(frm, text='Browse...', command=lambda k=key, t=kind: pick(k, t)).grid(row=i, column=2)
    frm.columnconfigure(1, weight=1)
    bar = ttk.Progressbar(frm, maximum=100)
    bar.grid(row=7, column=0, columnspan=3, sticky='ew', pady=8)
    status = ttk.Label(frm, text='')
    status.grid(row=8, column=0, columnspan=3, sticky='w')
    log = tk.Text(frm, height=16, wrap='none')
    log.grid(row=9, column=0, columnspan=3, sticky='nsew')
    frm.rowconfigure(9, weight=1)
    btns = ttk.Frame(frm)
    btns.grid(row=10, column=0, columnspan=3, sticky='e', pady=(8, 0))

    def say(msg):
        lines.put(msg)

    def pump():
        step = None
        while not lines.empty():
            ln = lines.get()
            log.insert('end', ln + '\n')
            log.see('end')
            s = STEP.search(ln)
            if s:
                step = f'Step {s.group(1)} of {s.group(2)}'
                status.config(text=step)
            m = PERCENT.search(ln)
            if m:
                bar['value'] = min(100, int(m.group(1)))
        play_btn.config(state='normal' if newest_game(v['install'].get()) and not busy['on'] else 'disabled')
        root.after(200, pump)

    def do_install():
        dump, keys = v['dump'].get().strip(), v['keys'].get().strip()
        if not (os.path.isfile(dump) and os.path.isfile(keys)):
            messagebox.showerror('Setup', 'Choose your game dump and your keys file first.')
            return
        inputs = [dump, keys] + ([v['firmware'].get().strip()] if v['firmware'].get().strip() else [])
        save_config({k: x.get() for k, x in v.items()})
        busy['on'] = True
        install_btn.config(state='disabled')

        def work():
            try:
                rc = install_game(v['install'].get(), inputs, v['update'].get().strip() or None, say, os.getpid())
                say('Setup finished: press Play.' if rc == 0 else f'Setup stopped (exit {rc}): the lines above say why. '
                    'Press Install again to continue from there.')
            except Exception as e:
                say(f'ERROR: {e}')
            busy['on'] = False
            root.after(0, lambda: install_btn.config(state='normal'))
        threading.Thread(target=work, daemon=True).start()

    def do_play():
        save_config({k: x.get() for k, x in v.items()})
        threading.Thread(target=lambda: play(v['install'].get(), say), daemon=True).start()
    install_btn = ttk.Button(btns, text='Install', command=do_install)
    install_btn.pack(side='left', padx=4)
    play_btn = ttk.Button(btns, text='Play', command=do_play)
    play_btn.pack(side='left', padx=4)
    root.after(200, pump)
    root.mainloop()


def main(argv):
    if '--play' in argv:
        return play(load_config().get('install') or DEFAULT_INSTALL)
    if '--setup' in argv:
        a = [x for x in argv if x != '--setup']
        install = load_config().get('install') or DEFAULT_INSTALL
        update = None
        for flag in ('--install', '--update'):
            if flag in a:
                i = a.index(flag)
                if flag == '--install':
                    install = os.path.abspath(a[i + 1])
                else:
                    update = os.path.abspath(a[i + 1])
                del a[i:i + 2]
        try:
            return install_game(install, [os.path.abspath(x) for x in a], update, print)
        except RuntimeError as e:
            print(f'ERROR: {e}')
            return 1
    try:
        window()
    except ImportError as e:   # (no Tk in a stripped Python: say what still works)
        print(f'no window ({e}): use --setup <dump> <keys> [--install DIR], then --play')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
