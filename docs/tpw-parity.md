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
| Mouse options | `--free-mouse` | none (relative mode always on while focused) |
| Screenshots | `--shot`, `--shot-after`, `--shot-every` | env vars only (`STL_SHOT`, `STL_SHOT_EVERY`) |
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
7. **Small parity options.** `--free-mouse`; `--shot`, `--shot-after` and
   `--shot-every` as arguments; a `README` section on playing (keys, F11,
   Ctrl+Cmd).
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
