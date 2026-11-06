# Star Trek: Legacy — Static Recompilation

A static recompilation of Mad Doc Software's **Star Trek: Legacy** (Bethesda,
2006), version 1.2, from its shipping Win32 binaries to native C, targeting
**macOS** with a Metal renderer. Built on the
[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain and its
`runtime/win32hle`, the same way as
[tpw-recomp](https://github.com/cpressland/tpw-recomp).

**Status: Phase 1 of [the plan](docs/PLAN.md).** The game's folder is built
from the disc image and the 1.2 patch with no Windows and no installer. The
lift, the host and the Direct3D 9 renderer come next.

## What you need

Nothing from the game is in this repository, and nothing generated from it.
You supply two files:

1. **The disc image**, `star_trek_legacy.iso` (2.2 GB,
   MD5 `30c793de689aa708e658af4473237054`).
2. **The 1.2 patch**, `star_trek_legacy_1_2_patch.exe` (5.3 MB,
   MD5 `d48cc0e5aeac1bc51df3d11ac59c22bb`).

`--download` fetches both from `files.cpressland.io` (`STL_DOWNLOAD_BASE`
for elsewhere), resuming a download that stopped, and checks their MD5s.

## The game folder

Python 3.10+ and one package, olefile (the rest of `requirements.txt` is for
the lift, later):

```
uv venv .venv && uv pip install --python .venv/bin/python -r requirements.txt
.venv/bin/python -I tools/install/install.py --iso star_trek_legacy.iso --patch star_trek_legacy_1_2_patch.exe
```

or `--download` in place of `--iso` and `--patch`. It takes under half a
minute from local files and writes `game/` (3.1 GB, 14,490 files) into
`~/Library/Application Support/Star Trek Legacy` (`--dest` for elsewhere),
with `Legacy.exe` checked against the 1.2 build's MD5
(`853616c05b540bf81afbd6e7b738d2f0`). Run again, it does nothing unless the
game folder is gone or its steps changed; `--status` says what a run would
redo.

What it does, each part usable on its own:

| Tool | What |
|---|---|
| `tools/disc/iso.py` | the files of an ISO 9660 image (from tpw-recomp) |
| `tools/disc/msi.py` | a Windows Installer database's tables, streams, and where it installs each file (`--tree`) |
| `tools/disc/cab.py` | a Microsoft cabinet set: spanned across files, read in place inside the disc image, stored, MSZIP or LZX |
| `tools/disc/lzx.py` | LZX decompression, in plain Python |
| `tools/disc/isstream.py` | the files inside an InstallShield 11 `setup.exe` (the patch) |
| `tools/install/install.py` | all of it, in order: the disc's database and cabinets, then the patch's |

The disc is an InstallShield 11 install: `Star Trek Legacy.msi` says where
each file goes, and `Data1.cab` + `Data11.cab` hold them under database
keys. The patch replaces 15 whole files (no deltas), from a cabinet inside
its own database inside its setup program. The `DirectX/` redistributable
on the disc is left out. Neither `Legacy.exe` 1.0 nor 1.2 has copy
protection. See [docs/PLAN.md](docs/PLAN.md) for the whole survey.

## License

MIT for this repository's own code. Star Trek: Legacy is © CBS Studios and
Bethesda Softworks; none of its files, and nothing generated from them, is in
this repository.
