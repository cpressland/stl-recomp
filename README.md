# Star Trek: Legacy — Static Recompilation

A static recompilation of Mad Doc Software's **Star Trek: Legacy** (Bethesda,
2006), version 1.2, from its shipping Win32 binaries to native C, targeting
**macOS** with a Metal renderer. Built on the
[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain and its
`runtime/win32hle`, the same way as
[tpw-recomp](https://github.com/cpressland/tpw-recomp).

**Status: Phase 4 of [the plan](docs/PLAN.md), early but drawing.** The
game's folder is built from the disc image and the 1.2 patch with no
Windows and no installer; the game and the three Microsoft DLLs it brings
are lifted to C; and the native host runs it on Direct3D 9 implemented on
Metal, with the game's shaders translated to Metal's: the main menu, and
the first campaign mission's ship, planets, nebula and HUD, draw. Not yet:
movies (skipped), fixed-function drawing, Doppler and sound cones, and a
lot of play-testing.

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

## The lift

The game's modules are lifted, each at its own fixed base (`tools/gen/modules.py`)
so that one dispatch table holds them all, and a call through one module's
import slot finds the other module's lifted function once the host binds it:

| Module | Base | What |
|---|---|---|
| `Legacy.exe` 1.2 | `0x00400000` | the game: Mad Doc's Mad3D engine, MSVC 7.1 |
| `d3dx9_27.dll` | `0x01000000` (moved from `0x00400000`) | D3DX, August 2005: the HLSL compiler and effects the game builds its shaders with at startup, texture loading, maths |
| `msvcr71.dll`, `msvcp71.dll` | their own | the Visual C++ 7.1 C runtime and C++ standard library |
| `missions/*.dsl`, `*.drl` (38) | `0x02000000` up, 256 KB apart (all moved from `0x10000000`) | each mission's script (an S) and its runner (an R), which the game loads with `LoadLibrary` as the mission starts: the briefing, the objectives, the mission's events |

```
.venv/bin/python -I tools/install/install.py --dest DIR     # after the game folder: analyse and lift
```

or by hand, with pcrecomp at `app/pcrecomp.lock`'s commit beside this repository
(below):

```
.venv/bin/python -I tools/gen/analyse.py GAME work/analysis   # catalogs, a few minutes
.venv/bin/python run_lift.py --work work/analysis --out work/gen
cmake -S . -B build-macos -G Ninja && cmake --build build-macos   # compiles the lifted C
```

Guest C++ exceptions are caught by the guest's own catch blocks: msvcr71's
`_CxxThrowException` is pcrecomp's `hle_eh_throw` (win32hle `eh32.c`), and
every function with a catch gets a landing pad (`tools/lift/eh32.py`),
including the ones msvcp71 opens through `__EH_prolog`.

## Playing it

`tools/install/install.py` (below) ends with the compiled game in
`DIR/build/host`; then:

```
DIR/build/host/stl --game DIR/game --run --windowed      # or full screen without --windowed
```

F11 toggles full screen, F12 the scaling, Ctrl+Cmd lets go of the mouse.

**Star Trek Legacy.app** does all of it: `app/build-app.sh` makes
`build-app/Star Trek Legacy.app`, which downloads the two files (or takes
your own), builds the game into `~/Library/Application Support/Star Trek
Legacy` (about an hour the first time) and opens on Play. It needs only
Xcode's command line tools. It is tpw-recomp's launcher; see that README
for its options (`STL_HOME`, `STL_ISO`, `STL_PATCH`, `STL_SNAPSHOT`, ...).

For testing, headless, with scripted input and screenshots:

```
stl --game DIR/game                                          # map and bind: says what is missing
STL_SHOT=shots STL_SHOT_EVERY=5000 stl --game DIR/game --run --headless --mute --watchdog 200 \
    --click 512,408@75 --click 512,393@95                    # the menu, then the first mission
```

Input is `--move x,y@s`, `--click x,y@s`, `--rclick x,y@s`, `--key vk@s`, and
`--keydown vk@s` / `--keyup vk@s` for a key held (E, 0x45, for the power
wheel), and `--rel dx,dy@s` for the mouse moved by dx, dy (as in the window,
where the mouse is held as relative motion: the camera turns past the edge),
at `s` seconds from the start, in the game's 1024x768.

`tools/test/smoke.sh BUILD/stl GAME` does that and checks it: the intro,
the menu and the first mission all draw, the ship takes the throttle,
nothing faults (about four minutes).

`STL_SHOT=DIR` writes the back buffer every `STL_SHOT_EVERY` frames;
`STL_TRACE_FRAME=N` lists every draw, clear and StretchRect of frame N;
`STL_TRACE_LAYOUT=1` how each pipeline matched shader inputs to the vertex
declaration; `STL_CHECK_WINDOW=1` what the window shows.

`--verbose` lists every import still a stub and every Direct3D 9 call as
it is made; at exit the run prints how often each Direct3D 9 method was
called. `STL_DUMP_SHADERS=DIR` writes each shader D3DX compiles,
`HLE_TRACE_EH=1` each C++ throw and where it lands. The game's saves and
settings go to a drive `U:`, `DIR/User`, beside the game folder.

What the host is (`src/host`): `loader.c` maps the modules at their
bases (the missions' scripts attached when the game loads them) and binds them to each other (D3DX's `msvcrt.dll` imports to
msvcr71) and to win32hle; `wide.c` the Unicode calls; `shims.c` the rest
win32hle lacks; `setjmp.c` D3DX's `setjmp`/`longjmp` on eh32's landing
pads; `d3d9.c` Direct3D 9's interfaces and state, `d3d9_metal.m` the same
on Metal, `dxso.c` the shaders (D3D9 bytecode to Metal Shading Language,
checked by `tools/dxso`); `cpu.c` the CPU it reports (a Pentium III);
`script.c` scripted input.

## pcrecomp: pending merges

This builds against pcrecomp's `main` plus branches still in review, merged
into one commit that `app/pcrecomp.lock` pins: the `stl-lock` branch of
[cpressland/pcrecomp](https://github.com/cpressland/pcrecomp). Clone it
beside this repository at that commit:

```
git clone https://github.com/cpressland/pcrecomp ../pcrecomp
git -C ../pcrecomp checkout "$(sed -n 's/^ref=//p' app/pcrecomp.lock)"
```

What `stl-lock` adds to `main`:

| Branch | What | Needed? |
|---|---|---|
| `win32hle-eh32` | guest C++ exceptions on win32hle | **Yes**: the engine and its runtime throw |
| `lift-eh32-eh-prolog` | landing pads for frames opened through `__EH_prolog` | **Yes**: msvcp71's catches |
| `win32hle-dinput8` | DirectInput 8, on `win32hle-dinput` | **Yes**: the game's mouse and keyboard |
| `win32hle-dx9-system-dlls` | `d3d9`, `dinput8` and XInput load as system DLLs | **Yes**: the game loads `d3d9.dll` at run time |
| `win32hle-dsound3d` | DirectSound 3D, on `win32hle-dsound-cursor` | **Yes**: the game places every sound |
| `win32hle-warp-picture-size` | SetCursorPos puts the pointer by the picture's size | **Yes**: the power distribution wheel (E) centres the cursor |
| `win32hle-dinput-relative-shared` | a host can give a non-exclusive DirectInput mouse movement alone (`hle_dinput_relative_shared`), on `tpw-local` | **Yes**: the camera turns past the window's edge |
| `tpw-local` | tpw-recomp's: DirectInput, Metal, the mouse held in the picture and its movement past the edge (`hle_input_raw`), the DirectSound cursor | **Yes**: the host's window, mouse and sound |

To make a new lock (after one of these changes, or `main` moves), merge
them again onto `origin/main` in that order, push the result to the fork's
`stl-lock`, and put its commit in `app/pcrecomp.lock`. The merges conflict
in three places, each kept both ways: `runtime/win32hle/README.md` (both
rows), `dinput.c`'s device fields (`exclusive` and `v8`), and `screen.c`'s
`warp()` (the picture's size, and no warp while relative). When a branch
is merged upstream, drop it from this table.

## License

MIT for this repository's own code. Star Trek: Legacy is © CBS Studios and
Bethesda Softworks; none of its files, and nothing generated from them, is in
this repository.
