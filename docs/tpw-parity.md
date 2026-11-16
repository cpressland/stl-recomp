# What tpw-recomp has that this project hasn't (2026-11-08)

tpw-recomp (Theme Park World) is the project this one was modelled on. Since
stl-recomp was started from it, it has gone on to ship: a released Mac app
with updates, a Linux Flatpak, and a few player-facing options. This is what
stl-recomp lacks by comparison, and the order to close it in.

## Where the two stand

| | tpw-recomp | stl-recomp |
|---|---|---|
| Game | playable, released | playable start to the first mission's objectives; no full playthrough yet |
| Mac app | `Theme Park World.app`, released as a DMG, a zip and `install.sh` | `Star Trek Legacy.app` builds locally (`app/build-app.sh`); never released |
| Updates | Sparkle, signed feed (`appcast.xml`), key in `app/sparkle-public-key.txt` | Sparkle key made (public half in `app/sparkle-public-key.txt`), feed from the release |
| Releases | `.github/workflows/release.yml` + `app/release.sh` on a `v*` tag | `.github/workflows/release.yml` + `app/release.sh` on a `v*` tag (2026-11-08; first release waits on the secret) |
| pcrecomp pin | `app/pcrecomp.lock`: one commit on the fork with the pending merges | `app/pcrecomp.lock`: the fork's `stl-lock` (done 2026-11-08) |
| Upstream | six PRs open on sp00nznet/pcrecomp (#158–#164) | seven branches on the fork, no PRs opened |
| Linux | Flatpak (x86-64, arm64), `app/linux/launcher.c`, CI, published via cpressland/flatpak | macOS only; the D3D9 backend is Metal-only |
| First-run build | ~10 minutes, 3 GB | ~12 minutes, 8 GB at its peak (was ~1 hour; done 2026-11-08) |
| Movies in the app | n/a (TPW's are Smacker, in win32hle) | play: the app carries a static Bink-only ffmpeg (`app/toolchain.sh`; done 2026-11-08) |
| Resolution | `--resolution WxH\|native\|options`, remembered; first launch is native | `--resolution WxH\|native\|options`, kept in `User/resolution.txt`; the app's first launch is native (2026-11-09) |
| Presenting | Metal, each flip's frame copied down without waiting (a ring of three), shown by screen.c | the same, since 2026-11-08 (it waited for the GPU each frame before: 40% of the headless frame rate) |
| Mouse options | `--free-mouse` | `--free-mouse` (2026-11-09) |
| Screenshots | `--shot`, `--shot-after`, `--shot-every` | the same (2026-11-09), and `STL_SHOT` by frames |
| Bring-up tracing | `src/host/trace.c` (`-DTPW_TRACE=ON`: first-hit, ring, argument traces) | `--trace` (imports only) |
| Linux file names | install.py and the host find files by any case | assumes macOS's case-insensitive disk |

## What doesn't carry over

- TPW's widescreen work (HUD anchoring, 4:3 movies, lift patches) exists
  because TPW is a 4:3 game. Star Trek: Legacy supports widescreen itself;
  it only needs the modes offered (below).
- QMixer, SafeDisc and RTPatch are TPW's own problems.

## Tasks, in order

1. ~~**Pin pcrecomp and push it.**~~ Done: `app/pcrecomp.lock` pins the
   fork's `stl-lock`, which also takes tpw-local's newer win32hle (its
   relative mouse replaced this project's own `win32hle-raw-mouse`).
2. ~~**Movies in the app.**~~ Done: `app/toolchain.sh` builds ffmpeg 7.1.1
   (pinned) with the Bink demuxer and decoders alone, static, 8 MB; the app
   passes it to `install.py --ffmpeg`, and CMake links it (`STL_FFMPEG`).
3. ~~**Release pipeline.**~~ Ported: `release.yml`, `app/release.sh`,
   `app/install.sh`, and a Sparkle key pair (private half in the login
   Keychain). A local `release.sh` run made the DMG, zip and a signed
   appcast. Left: the `SPARKLE_PRIVATE_KEY` repository secret, then a tag.
4. ~~**Cut the first-run build time.**~~ Done: 48 minutes to 12 (disc 17 s,
   analysis 241 s, lift 185 s, compile 265 s, on an M-series Mac).
   - disasm32 decodes each address once (pcrecomp `disasm32-decode-cache`):
     D3DX's analysis 1,263 s to 242 s, the same catalog byte for byte. One
     38,000-instruction body was being decoded once per each of its 1,114
     entries.
   - Those entries are switch arms, each lifted as its own copy of nearly
     the whole body; lifted functions over 3,000 lines in D3DX (10,000
     elsewhere) are compiled `optnone` (`run_lift.py`), which took the
     longest file from 24 minutes to under 2.
   Left, if wanted: lift the modules in parallel (the lift is one process,
   D3DX's 2 minutes of it), and stop lifting switch arms as copies at all.
5. ~~**Present without the readback.**~~ Done, as TPW does it (this note
   first said TPW draws straight to a `CAMetalLayer`; it doesn't): each
   Present copies the back buffer into one of three shared buffers without
   waiting, and the window shows the newest one finished. 11,400 frames to
   34,200 in the same 150-second windowed run. A `CAMetalLayer` of the
   game's own would save the last copy too, but would take screen.c's
   scaling, menus and mouse mapping with it: not worth it now.
6. ~~**Resolutions.**~~ Done. The game sizes its first device from its
   profile (`Admiral.PRF`, written when Video options are saved) or 1024x768,
   and takes whatever size that device is made at as its own, layout,
   projection and Video page included. So the host keeps a mode
   (`User/resolution.txt`: `--resolution`, or the last one chosen in the
   game) and makes the first device at it. D3D9 reports the Mac's desktop as
   the display mode. The game's Video dropdown is its own nine sizes
   (`ui/graphicsSettings.xml`); other sizes come from `--resolution`.
7. ~~**Small parity options.**~~ Done: `--free-mouse`, `--shot FILE.bmp`
   with `--shot-after` and `--shot-every` (as TPW's), and a Controls section
   in the README. Found on the way: quitting crashed (fixed with v0.4.0).
8. **Test more of the game.** Smoke runs that start each campaign mission
   and the skirmish and multiplayer setup screens, checking for faults and
   that the mission script loads. Do this before the first release so it
   catches regressions in the 38 mission scripts.
9. **Open the upstream PRs** on sp00nznet/pcrecomp for the fork branches
   in the README's table (and tpw-local's relative mouse, which has no PR
   yet), as TPW did, so the lock can eventually be plain `main`.
10. **Linux.** The largest item: a D3D9 backend that isn't Metal (Vulkan, or
    OpenGL 3.3 to start), file lookup by any case, then TPW's Flatpak, its
    launcher and CI. Worth doing after the Mac release is solid.
11. **Controller.** The game ships `xinput9_1_0.dll` and supports an Xbox
    360 pad. Answering XInput from SDL's game controllers would give
    controller play on Mac (and the Steam Deck, after Linux).
12. **Bring-up tracing.** Port TPW's `trace.c` for the next hard bug. It's
    only needed when one turns up.

## Since then (reviewed 2026-11-16)

tpw-recomp has 24 commits since 2026-11-08. Its pcrecomp is now the fork's
`tpw` branch (`3fc41e5`): upstream main, which has merged five of the
pending PRs and added its own x87 and indirect-call speed-ups, plus each
change still in review as its own PR (#163, #180-#187). `stl-lock` is
behind all of it. What that, and the rest, means here:

| tpw-recomp has | stl-recomp | Worth it here? |
|---|---|---|
| **The speed-ups.** `RECOMP_X87_RING`, `RECOMP_X87_LOCAL`, `RECOMP_RESOLVE_CACHE=16384`, `RECOMP_ICALL_SITE_CACHE` (upstream #174, #175), a hand-over on a condition variable, `Sleep(0)` that sleeps: a park went from 119% of a core to 33% | none of them: `stl-lock` predates them, and CMake sets only `RECOMP_LOCAL_REGS` | **Yes, first.** Legacy.exe makes ~4 million indirect calls a second (watchdog counts); the call caches are aimed at exactly that |
| **A frame-rate cap.** win32hle's pacer holds a game near 60 a second, in step with the display | none: Present returns at once. A window ran ~228 frames a second in a test, all of it CPU and GPU spent | **Yes.** D3D9 doesn't flip through screen.c, so the pacer can't see it: Present has to pace itself (or wait for the display) |
| **The mouse wheel**, as `WM_MOUSEWHEEL` and DirectInput's z axis (pcrecomp `f6b1037`) | not passed on: the game never sees the wheel | **Likely.** The game's camera zooms, and its DIMOUSESTATE has the z axis |
| **The extended-key bit** for the arrows, Delete, Home, Insert, Page Up/Down (pcrecomp `3fc41e5`): without it a game reads them as the keypad | missing | **Likely.** Not tested; any of the game's bindings on those keys may be off |
| **A controller** (`src/host/pad.c`, SDL GameController turned into mouse and keys, through pcrecomp's poll hook `f6f05f6`) | none | **Yes, but differently.** Legacy supports an Xbox 360 pad itself, through XInput, which nothing answers yet (`GetProcAddress(xinput…)` finds nothing). XInputGetState from SDL's controller is a better fit than TPW's mapping |
| **Linux**: a Flatpak and now an AppImage carrying its own compiler (zig), Direct3D through Vulkan (`d3d_vulkan.c`, DirectX 6 only) | macOS only | **Later.** TPW's Vulkan renderer is DirectX 6's; Legacy's D3D9 would need its own Vulkan backend, as `d3d9_metal.m` is for Metal |
| **Versions from `v` tags only** (`git describe --match 'v[0-9]*'`) | `git describe --tags` | Only once a non-version tag appears (TPW's came from the Flatpak CI). Trivial to match |
| **A first-launch fix**: a file the host writes into the game's folders is announced (`hle_path_changed()`), or the path layer's cached listing hides it | not affected: `User/resolution.txt` is read by the host, never the game | No |
| Widescreen dialogs, Halloween's lightning, QMixer, the park camera | — | No: TPW's own game |

Also: the repositories now sit in `recomps/`, and pcrecomp doesn't.
stl-recomp's build looks for pcrecomp beside it (`../pcrecomp`), so from
`recomps/stl-recomp` a build needs `--pcrecomp ~/dev/github.com/pcrecomp`
(install.py) or `-DPCRECOMP=` (CMake), or a link.

### What to do, in order

1. ~~Rebuild the lock on the fork's `tpw` branch, with the four flags.~~
   Done (2026-11-16): the fork's `stl` branch is `tpw` plus this game's
   eight commits, and CMake sets the flags. A windowed mission: 380 frames
   a second at 116% of a core before, 490 at 118% after (328 to 416
   frames per CPU second, 27% more). The wheel reaches the game (its
   buffered z axis reads ±360 for 3 notches); whether it zooms wants a
   hand on a mouse. The camera's movement past the edge still arrives
   whole (a 3000-pixel move read as 3000).
2. ~~Pace Present to the display.~~ Done (2026-11-16): Present ends with
   `hle_screen_flip` (pcrecomp `win32hle-screen-flip`), win32hle's pacer.
   The same windowed minute: 490 frames a second at 118% of a core before,
   60 at 40% after, held 2 refreshes of the 120 Hz display, none late.
   `HLE_MAX_FPS` changes the cap; headless runs stay unpaced.
3. ~~XInput from SDL's game controllers.~~ Done (2026-11-16), and not
   XInput: the game never loads it. It looks for a controller through
   DirectInput 8 (EnumDevices, game controllers) and reads it as Windows
   shows an Xbox 360 pad (DIJOYSTATE2, axes -100..100). pcrecomp's
   `win32hle-dinput-pad` gives DirectInput that pad, and `src/host/pad.c`
   feeds it from SDL's GameController (`STL_VPAD=FILE` for a virtual one).
   A virtual pad's d-pad and A took the main menu to Options.
4. Then the list above resumes: tests of every mission, upstream PRs,
   Linux (now with a D3D9 Vulkan backend to write), tracing.
