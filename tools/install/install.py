"""From the disc image and the 1.2 patch to the game's folder, in one go.

    .venv/bin/python -I tools/install/install.py --iso star_trek_legacy.iso --patch star_trek_legacy_1_2_patch.exe
    .venv/bin/python -I tools/install/install.py --download

Into one folder (by default the Mac's Application Support, where Star Trek
Legacy.app will look):

    game/          the installed game, patched to 1.2: what the Windows
                   installer and the patch would have put in Program Files
    install.json   what each part was made from

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
redo, as JSON. The lift and the build join these steps in later phases
(docs/PLAN.md).

Progress goes to stdout as plain lines; `@step i n title`, `@progress
fraction` and `@error text` lines are for a front end to show.
"""
import argparse
import hashlib
import json
import os
import shutil
import ssl
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

    def keys(self):
        return {'data': f'data-{DATA_VERSION}'}

    def stale(self, keys):
        """The parts out of date, in order: one stale part makes all after it so."""
        if not os.path.isfile(os.path.join(self.game, GAME_EXE)):
            self.state.pop('data', None)
        out = []
        for k in ('data',):
            if out or self.state.get(k) != keys[k]:
                out.append(k)
        return out

    # ---- the steps -------------------------------------------------------

    def check(self):
        try:
            import olefile  # noqa: F401
        except ImportError:
            raise Failed('this Python lacks olefile (README: Python packages)') from None
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
                 *([('Downloading the game', self.download, None)] if fetch else []),
                 ('Installing and patching the game', self.disc, 'data')]
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
        say(f'done. The game is in "{self.game}".')


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
    ap.add_argument('--force', action='store_true', help='redo every step')
    ap.add_argument('--status', action='store_true', help='print what a run would redo, as JSON')
    a = ap.parse_args()
    try:
        if a.status:
            Install(a).status()
        else:
            Install(a).go()
    except Failed as e:
        say(f'@error {e}')
        say(f'error: {e}')
        sys.exit(1)


if __name__ == '__main__':
    main()
