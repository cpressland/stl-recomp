"""From the disc image and the 1.2 patch to the game's folder, in one go.

    .venv/bin/python -I tools/install/install.py --iso star_trek_legacy.iso --patch star_trek_legacy_1_2_patch.exe
    .venv/bin/python -I tools/install/install.py --download

Into one folder (by default the Mac's Application Support, where Star Trek
Legacy.app will look):

    game/            the installed game, patched to 1.2: what the Windows
                     installer and the patch would have put in Program Files
    build/src/       the sources it builds from, copied from this repository
                     and pcrecomp (SOURCES), so the build's paths never move
    build/analysis/  each module's function catalog and the rest of the
                     lift's inputs (tools/gen/analyse.py)
    build/gen/       the lifted C of all four modules (run_lift.py)
    build/host/      the compiled game: stl, the program, and libstl.dylib,
                     the same game for the app to load (stl_main)
    install.json     what each part was made from

No installer is run. The disc is an InstallShield 11 install: a Windows
Installer database (`Star Trek Legacy.msi`) whose tables say where each
file goes, and one cabinet set spanning `Data1.cab` and `Data11.cab` that
holds the files by their database keys. Both are read where they lie in the
disc image (tools/disc/iso.py, msi.py, cab.py), and the files are written
straight into the game folder by their real names. The redistributable
DirectX installer the disc carries (`DirectX/`) is left out.

The patch is an InstallShield setup.exe (tools/disc/isstream.py) around
another database, whose own cabinet (LZX, tools/disc/lzx.py) holds 15 whole
files to put in place of the disc's: no binary deltas.

Run again, it redoes only what is out of date: the disc's steps run again
only when they change (DATA_VERSION) or the game folder is gone, and then
need the disc image and the patch again. --status says what a run would
redo, as JSON: after a lifter fix only the lift, after an analysis fix the
analysis and the lift. The compile and the app join these steps in later
phases (docs/PLAN.md); the compile is ninja's, so after a host fix only
what changed is rebuilt.

Progress goes to stdout as plain lines; `@step i n title`, `@progress
fraction` and `@error text` lines are for a front end to show.
"""
import argparse
import filecmp
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'disc'))

import cab  # noqa: E402
import iso  # noqa: E402
import isstream  # noqa: E402
import msi  # noqa: E402

ISO_MD5 = '30c793de689aa708e658af4473237054'
PATCH_MD5 = 'd48cc0e5aeac1bc51df3d11ac59c22bb'
# Legacy.exe as the 1.2 patch installs it ("43.0.0.0", 9 Feb 2007).
GAME_MD5 = '853616c05b540bf81afbd6e7b738d2f0'
GAME_EXE = 'Legacy.exe'
DISC_MSI = 'Star Trek Legacy.msi'
PATCH_MSI = 'Star Trek Legacy Patch v1.2.msi'
# The disc's files not installed: the DirectX redistributable's installer.
SKIP = ('DirectX/',)
# Bumped when the disc's steps change what they make.
DATA_VERSION = 1
DEFAULT_DEST = os.path.expanduser('~/Library/Application Support/Star Trek Legacy')
# What a player's run writes into the game folder, carried over a new one.
# (None known yet: filled in once the game runs and we see where it saves.)
KEEP = ()
SPACE_NEEDED = 6 << 30       # the game folder (3.3 GB) and the downloads (2.2 GB)

# What the build needs of each tree: what the app will carry, and what is staged.
SOURCES = {'stl': ('CMakeLists.txt', 'run_lift.py', 'src', 'tools'),
           'pcrecomp': ('runtime', 'tools', 'LICENSE')}

SDL_FRAMEWORKS = ('SDL2.framework', 'SDL2_ttf.framework')

# --download: the two files, from where this project keeps copies of them
# (STL_DOWNLOAD_BASE: elsewhere). Checked against the same MD5s as ever.
DOWNLOAD_BASE = os.environ.get('STL_DOWNLOAD_BASE', 'https://files.cpressland.io/public/stl-recomp')
DOWNLOADS = (('disc image', 'star_trek_legacy.iso', ISO_MD5, 'iso'),
             ('1.2 patch', 'star_trek_legacy_1_2_patch.exe', PATCH_MD5, 'patch'))


class Failed(Exception):
    pass


def say(*a):
    print(*a, flush=True)


def md5(path, progress=None):
    h, size, done = hashlib.md5(), os.path.getsize(path), 0
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done / size)
    return h.hexdigest()


def _files(top):
    """Every file under a folder (or the file itself), less Python's caches."""
    if os.path.isfile(top):
        return [top]
    return sorted(os.path.join(d, n) for d, ds, ns in os.walk(top) for n in ns
                  if '__pycache__' not in d.split(os.sep) and not n.endswith('.pyc') and n != '.DS_Store')


def tree_hash(*paths):
    """A hash of source files: what a step's output depends on."""
    h = hashlib.sha1()
    for top in paths:
        for p in _files(top):
            h.update(os.path.relpath(p, os.path.dirname(top)).encode())
            with open(p, 'rb') as f:
                h.update(f.read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:16]


def sync(src, rels, dst):
    """Make dst/<rel> match src/<rel>, writing a file only when its bytes
    differ (tpw-recomp's). Returns how many changed."""
    changed = 0
    for rel in rels:
        s, d = os.path.join(src, rel), os.path.join(dst, rel)
        want = {os.path.relpath(p, s) if os.path.isdir(s) else '' for p in _files(s)} if os.path.exists(s) else set()
        if os.path.isdir(d):
            for p in _files(d):
                if os.path.relpath(p, d) not in want:
                    os.remove(p)
                    changed += 1
        for r in sorted(want):
            sp, dp = os.path.join(s, r) if r else s, os.path.join(d, r) if r else d
            if os.path.isfile(dp) and filecmp.cmp(sp, dp, shallow=False):
                continue
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copyfile(sp, dp)
            shutil.copymode(sp, dp)
            changed += 1
    return changed


def find_pcrecomp(given):
    for p in (given, os.environ.get('PCRECOMP'), os.path.join(REPO, 'pcrecomp'),
              os.path.join(os.path.dirname(REPO), 'pcrecomp')):
        if p and os.path.isdir(os.path.join(p, 'runtime', 'win32hle')):
            return os.path.abspath(p)
    raise Failed('no pcrecomp found: give --pcrecomp, or clone it beside this repository (README)')


def _progress(lo, hi):
    """A progress callback for one stage, as part of the whole step."""
    last = [0.0]

    def report(done, total):
        if time.time() - last[0] > 0.25 or done == total:
            last[0] = time.time()
            say(f'@progress {lo + (hi - lo) * done / max(total, 1):.3f}')
    return report


def install_files(db, cabinets, out, skip=SKIP, progress=None):
    """Write the files a database installs, from its cabinet set, under out.
    Returns how many were written."""
    tree = {key: (path, size) for key, path, size, _seq in msi.install_tree(db)}
    written = [0]

    def write(key, data, mtime):
        if key not in tree:
            raise Failed(f'the cabinet holds {key}, which the database does not install')
        path, size = tree[key]
        if path.startswith(skip):
            return
        if len(data) != size:
            raise Failed(f'{path}: {len(data)} bytes in the cabinet, {size} in the database')
        cab.write_into(out)(path, data, mtime)
        written[0] += 1
    try:
        cab.CabinetSet(cabinets).extract(write, progress)
    except ValueError as e:
        raise Failed(f'the cabinets cannot be read: {e}') from None
    return written[0]


class Install:
    def __init__(self, a):
        self.a = a
        self.dest = os.path.abspath(a.dest)
        self.game = os.path.join(self.dest, 'game')
        self.pcrecomp = find_pcrecomp(a.pcrecomp)
        self.build = os.path.join(self.dest, 'build')
        self.src = os.path.join(self.build, 'src')
        self.stl_src = os.path.join(self.src, 'stl')
        self.pc_src = os.path.join(self.src, 'pcrecomp')
        self.analysis = os.path.join(self.build, 'analysis')
        self.gen = os.path.join(self.build, 'gen')
        self.host = os.path.join(self.build, 'host')
        self.env = dict(os.environ, PCRECOMP=self.pc_src)
        self.sdl = os.path.join(self.src, 'Frameworks') if a.sdl_frameworks else None
        self.ffmpeg = os.path.join(self.src, 'ffmpeg') if a.ffmpeg else None
        self.tmp = os.path.join(self.dest, 'tmp')
        self.state_path = os.path.join(self.dest, 'install.json')
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}

    # ---- bookkeeping ---------------------------------------------------

    def save_state(self):
        os.makedirs(self.dest, exist_ok=True)
        with open(self.state_path + '.new', 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(self.state_path + '.new', self.state_path)

    def run(self, *cmd):
        """A step's command; its output is shown as it comes."""
        say('$ ' + ' '.join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
        p = subprocess.run(cmd, cwd=self.stl_src, env=self.env)
        if p.returncode:
            raise Failed(f'{os.path.basename(cmd[1] if len(cmd) > 1 else cmd[0])} failed ({p.returncode})')

    def keys(self):
        """From the sources given (the repository's, or the app's), not the staged copy."""
        tools = os.path.join(self.pcrecomp, 'tools')
        gen = os.path.join(REPO, 'tools', 'gen')
        data = f'data-{DATA_VERSION}'
        analysis = tree_hash(os.path.join(tools, 'pe'), os.path.join(tools, 'cpp'), os.path.join(tools, 'disasm'),
                             *(os.path.join(gen, n) for n in ('analyse.py', 'data_code_ptrs.py', 'modules.py',
                                                              'rebase.py')))
        lift = tree_hash(os.path.join(tools, 'lift'), os.path.join(REPO, 'run_lift.py'))
        host = tree_hash(os.path.join(REPO, 'CMakeLists.txt'), os.path.join(REPO, 'src'),
                         os.path.join(self.pcrecomp, 'runtime'),
                         *(os.path.join(self.a.sdl_frameworks, f) for f in SDL_FRAMEWORKS if self.a.sdl_frameworks),
                         *((self.a.ffmpeg,) if self.a.ffmpeg else ()))
        return {'data': data, 'analysis': f'{data}/{analysis}', 'lift': f'{data}/{analysis}/{lift}',
                'host': f'{data}/{analysis}/{lift}/{host}'}

    def stale(self, keys):
        """The parts out of date, in order: one stale part makes all after it so."""
        if not os.path.isfile(os.path.join(self.game, GAME_EXE)):
            self.state.pop('data', None)
        if not os.path.exists(os.path.join(self.gen, 'recomp_dispatch.c')):
            self.state.pop('lift', None)
        if not os.path.exists(os.path.join(self.host, 'stl')):
            self.state.pop('host', None)
        out = []
        for k in ('data', 'analysis', 'lift', 'host'):
            if out or self.state.get(k) != keys[k]:
                out.append(k)
        return out

    # ---- the steps -------------------------------------------------------

    def find_tools(self):
        """CMake and Ninja: this Python's own packages first (an app's), then the PATH."""
        try:
            import cmake
            cm = os.path.join(cmake.CMAKE_BIN_DIR, 'cmake')
        except ImportError:
            cm = shutil.which('cmake')
        nj = os.path.join(os.path.dirname(sys.executable), 'ninja')
        if not os.access(nj, os.X_OK):
            nj = shutil.which('ninja')
        return cm, nj

    def check(self):
        self.cmake, self.ninja = self.find_tools()
        if not self.cmake or not self.ninja:
            raise Failed('no cmake or ninja (README: prerequisites)')
        if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
            raise Failed("no C compiler: install Xcode's command line tools (xcode-select --install)")
        if self.a.sdl_frameworks:
            for f in SDL_FRAMEWORKS:
                if not os.path.isdir(os.path.join(self.a.sdl_frameworks, f)):
                    raise Failed(f'no {f} in {self.a.sdl_frameworks}')
        elif subprocess.run(['pkg-config', '--exists', 'sdl2', 'SDL2_ttf']).returncode:
            raise Failed('pkg-config cannot find sdl2 and SDL2_ttf (README: prerequisites)')
        if self.a.ffmpeg and not os.path.isfile(os.path.join(self.a.ffmpeg, 'lib', 'libavcodec.a')):
            raise Failed(f'no static ffmpeg (lib/libavcodec.a) in {self.a.ffmpeg}')
        missing = [m for m in ('olefile', 'pefile', 'capstone')
                   if subprocess.run([sys.executable, '-I', '-c', f'import {m}'], capture_output=True).returncode]
        if missing:
            raise Failed(f'this Python lacks {", ".join(missing)} (README: Python packages)')
        os.makedirs(self.dest, exist_ok=True)
        free = shutil.disk_usage(self.dest).free
        if free < SPACE_NEEDED and not os.path.isdir(self.game):
            raise Failed(f'{free >> 20} MB free at {self.dest}; it needs about {SPACE_NEEDED >> 30} GB')

    def download(self):
        """The disc image and the patch, into downloads/: resumed where a last
        try stopped, checked, and kept only until the game folder is made."""
        folder = os.path.join(self.dest, 'downloads')
        os.makedirs(folder, exist_ok=True)
        try:
            ctx = ssl.create_default_context()
            urllib.request.urlopen(DOWNLOAD_BASE + '/' + DOWNLOADS[1][1], context=ctx, timeout=30).close()
        except ssl.SSLError:
            ctx = ssl.create_default_context(cafile='/etc/ssl/cert.pem')    # macOS's own roots
        except (urllib.error.URLError, OSError) as e:
            raise Failed(f'cannot reach {DOWNLOAD_BASE}: {getattr(e, "reason", e)}')
        sizes = {}
        for what, name, want, _ in DOWNLOADS:
            with urllib.request.urlopen(urllib.request.Request(f'{DOWNLOAD_BASE}/{name}', method='HEAD'),
                                        context=ctx, timeout=30) as r:
                sizes[name] = int(r.headers['Content-Length'])
        total, before = sum(sizes.values()), 0
        for what, name, want, arg in DOWNLOADS:
            path = os.path.join(folder, name)
            have = os.path.getsize(path) if os.path.exists(path) else 0
            if have > sizes[name]:
                os.remove(path)
                have = 0
            if have < sizes[name]:
                say(f'downloading the {what} ({sizes[name] >> 20} MB)...')
                req = urllib.request.Request(f'{DOWNLOAD_BASE}/{name}')
                if have:
                    req.add_header('Range', f'bytes={have}-')
                    say(f'  resuming at {have >> 20} MB')
                try:
                    with urllib.request.urlopen(req, context=ctx, timeout=60) as r, \
                            open(path, 'ab' if have and r.status == 206 else 'wb') as f:
                        if r.status != 206:
                            have = 0
                        last = 0.0
                        for chunk in iter(lambda: r.read(1 << 20), b''):
                            f.write(chunk)
                            have += len(chunk)
                            if time.time() - last > 0.25:
                                last = time.time()
                                say(f'@progress {(before + have) / total:.3f}')
                except (urllib.error.URLError, OSError) as e:
                    raise Failed(f'the download of the {what} stopped ({getattr(e, "reason", e)}); '
                                 'trying again carries on from there')
            say(f'checking the {what}...')
            got = md5(path)
            if got != want:
                os.remove(path)
                raise Failed(f'the downloaded {what} is not the expected file (MD5 {got[:8]}…, '
                             f'wanted {want[:8]}…); trying again downloads it afresh')
            setattr(self.a, arg, path)
            before += sizes[name]
        say('@progress 1')

    def need_inputs(self):
        for what, path, want in (('disc image', self.a.iso, ISO_MD5), ('1.2 patch', self.a.patch, PATCH_MD5)):
            if not path:
                raise Failed('the game must be set up from the disc: give --iso and --patch, or --download')
            if not os.path.isfile(path):
                raise Failed(f'no {what} at {path}')
            say(f'checking the {what}...')
            got = md5(path, lambda f: say(f'@progress {f:.3f}'))
            if got != want:
                raise Failed(f'{os.path.basename(path)} is not the {what} this needs: check it is the '
                             f'right file, downloaded whole (MD5 {got[:8]}…, wanted {want[:8]}…)')

    def disc(self):
        """Install from the disc, patch to 1.2: a new game folder, saves carried over."""
        self.need_inputs()
        shutil.rmtree(self.tmp, ignore_errors=True)
        new = os.path.join(self.tmp, 'game')
        os.makedirs(new)

        say('installing from the disc...')
        entries = {p: (lba, ln) for p, lba, ln, _ in iso.files(self.a.iso)}

        def from_iso(name):
            if name not in entries:
                raise Failed(f'the disc image has no {name}')
            lba, ln = entries[name]
            return cab.Part(self.a.iso, lba * iso.SECTOR, ln, name)
        db = msi.Database(from_iso(DISC_MSI).read(0, entries[DISC_MSI][1]))
        media = sorted(db.table('Media'), key=lambda m: m['DiskId'])
        n = install_files(db, [from_iso(m['Cabinet']) for m in media if m['Cabinet']], new,
                          progress=_progress(0, 0.95))
        say(f'{n} files')

        say('applying the 1.2 patch...')
        with open(self.a.patch, 'rb') as f:
            setup = dict(isstream.files(f.read()))
        if PATCH_MSI not in setup:
            raise Failed(f'the patch has no {PATCH_MSI}')
        pdb = msi.Database(setup[PATCH_MSI])
        parts = []
        for m in sorted(pdb.table('Media'), key=lambda m: m['DiskId']):
            if not (m['Cabinet'] or '').startswith('#'):
                raise Failed(f'the patch wants an outside cabinet, {m["Cabinet"]}')
            parts.append(cab.Part(msi.embedded_cabinet(pdb, m['Cabinet']), label=m['Cabinet']))
        n = install_files(pdb, parts, new, progress=_progress(0.95, 1))
        say(f'{n} files replaced')

        exe = os.path.join(new, GAME_EXE)
        got = md5(exe) if os.path.exists(exe) else 'missing'
        if got != GAME_MD5:
            raise Failed(f'{GAME_EXE} is not the 1.2 game (MD5 {got}, wanted {GAME_MD5})')
        for rel in KEEP:
            old = os.path.join(self.game, rel)
            if os.path.exists(old):
                dst = os.path.join(new, rel)
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.replace(old, dst)
        if os.path.exists(self.game):
            os.replace(self.game, os.path.join(self.tmp, 'old-game'))
        os.replace(new, self.game)
        shutil.rmtree(self.tmp, ignore_errors=True)
        shutil.rmtree(os.path.join(self.dest, 'downloads'), ignore_errors=True)

    def stage(self):
        """The sources, copied to where the build always finds them."""
        n = sync(REPO, SOURCES['stl'], self.stl_src) + sync(self.pcrecomp, SOURCES['pcrecomp'], self.pc_src)
        # The app's frameworks are copied whole, links and all, when they differ at all.
        for f in SDL_FRAMEWORKS if self.sdl else ():
            src, dst = os.path.join(self.a.sdl_frameworks, f), os.path.join(self.sdl, f)
            if not os.path.isdir(dst) or tree_hash(src) != tree_hash(dst):
                shutil.rmtree(dst, ignore_errors=True)
                shutil.copytree(src, dst, symlinks=True)
                n += 1
        # ...and the app's ffmpeg likewise (the movies' decoders).
        if self.ffmpeg and (not os.path.isdir(self.ffmpeg) or tree_hash(self.a.ffmpeg) != tree_hash(self.ffmpeg)):
            shutil.rmtree(self.ffmpeg, ignore_errors=True)
            shutil.copytree(self.a.ffmpeg, self.ffmpeg, symlinks=True)
            n += 1
        say(f'{n} source files changed')

    def analyse(self):
        """Each module's headers, C++ classes, function catalog, code in data."""
        shutil.rmtree(self.analysis, ignore_errors=True)
        say('cataloguing the modules\' functions: the game, its DLLs, the missions\' scripts (a few minutes)...')
        self.run(sys.executable, '-I', os.path.join(self.stl_src, 'tools', 'gen', 'analyse.py'),
                 self.game, self.analysis)

    def lift(self):
        # run_lift.py replaces only the files whose bytes changed, so the
        # compile after it redoes only those: the folder is kept.
        self.run(sys.executable, os.path.join(self.stl_src, 'run_lift.py'), '--work', self.analysis, '--out', self.gen)

    def compile(self):
        config = [self.stl_src, self.pc_src, self.sdl or 'pkg-config', self.ffmpeg or '', self.cmake]
        if self.state.get('host_config') != config:
            shutil.rmtree(self.host, ignore_errors=True)
        if not os.path.exists(os.path.join(self.host, 'build.ninja')):
            self.run(self.cmake, '-S', self.stl_src, '-B', self.host, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                     f'-DCMAKE_MAKE_PROGRAM={self.ninja}', f'-DPCRECOMP={self.pc_src}', f'-DGEN_DIR={self.gen}',
                     f'-DSTL_SDL_FRAMEWORKS={self.sdl or ""}', f'-DSTL_FFMPEG={self.ffmpeg or ""}')
            self.state['host_config'] = config
            self.save_state()
        say('compiling (a few minutes the first time; after a fix, what changed)...')
        self.run(self.cmake, '--build', self.host)

    # ---- the order -------------------------------------------------------

    def status(self):
        stale = self.stale(self.keys())
        say(json.dumps({'ready': not stale, 'stale': stale, 'needs_disc': 'data' in stale}))

    def go(self):
        keys = self.keys()
        if self.a.force:
            self.state = {}
        stale = self.stale(keys)
        fetch = 'data' in stale and self.a.download and not (self.a.iso and self.a.patch)
        steps = [('Checking the tools', self.check, None),
                 ('Copying the sources', self.stage, None),
                 *([('Downloading the game', self.download, None)] if fetch else []),
                 ('Installing and patching the game', self.disc, 'data'),
                 ('Analysing the game', self.analyse, 'analysis'),
                 ('Translating the game to C', self.lift, 'lift'),
                 ('Compiling', self.compile, 'host')]
        todo = [s for s in steps if s[2] is None or s[2] in stale]
        for i, (title, fn, key) in enumerate(todo, 1):
            say(f'@step {i} {len(todo)} {title}')
            say(f'==> [{i}/{len(todo)}] {title}')
            t = time.time()
            if key:
                self.state.pop(key, None)     # half-made until it finishes
                self.save_state()
            fn()
            if key:
                self.state[key] = keys[key]
                self.save_state()
            say(f'    {title.lower()}: {time.time() - t:.0f} s')
        say('@done')
        say('done. Play from a terminal:')
        say(f'  "{os.path.join(self.host, "stl")}" --game "{self.game}" --run --windowed')


def export(out, pcrecomp):
    """The sources the app carries: SOURCES of this repository and pcrecomp."""
    n = sync(REPO, SOURCES['stl'], os.path.join(out, 'stl')) + \
        sync(find_pcrecomp(pcrecomp), SOURCES['pcrecomp'], os.path.join(out, 'pcrecomp'))
    say(f'{n} files to {out}')


def main():
    if os.environ.get('STL_PGROUP'):
        os.setpgrp()       # the app stops the whole build through the group
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('--iso', help='star_trek_legacy.iso (needed the first time)')
    ap.add_argument('--patch', help='star_trek_legacy_1_2_patch.exe (needed the first time)')
    ap.add_argument('--download', action='store_true',
                    help='with no --iso and --patch, download both (STL_DOWNLOAD_BASE)')
    ap.add_argument('--dest', default=DEFAULT_DEST, help='where it goes (default: %(default)s)')
    ap.add_argument('--pcrecomp', help='the pcrecomp checkout (default: ./pcrecomp, then ../pcrecomp)')
    ap.add_argument('--sdl-frameworks', help='a folder with SDL2.framework and SDL2_ttf.framework '
                    '(default: SDL from pkg-config)')
    ap.add_argument('--ffmpeg', help='a static ffmpeg, include/ and lib/, for the movies '
                    '(default: pkg-config\'s, if any; app/toolchain.sh builds one)')
    ap.add_argument('--export-sources', metavar='DIR', help='copy the sources the build needs to DIR, and stop')
    ap.add_argument('--force', action='store_true', help='redo every step')
    ap.add_argument('--status', action='store_true', help='print what a run would redo, as JSON')
    a = ap.parse_args()
    if a.sdl_frameworks:
        a.sdl_frameworks = os.path.abspath(a.sdl_frameworks)
    if a.ffmpeg:
        a.ffmpeg = os.path.abspath(a.ffmpeg)
    try:
        if a.export_sources:
            export(a.export_sources, a.pcrecomp)
        elif a.status:
            Install(a).status()
        else:
            Install(a).go()
    except Failed as e:
        say(f'@error {e}')
        say(f'error: {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
