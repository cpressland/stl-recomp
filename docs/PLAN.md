# Star Trek: Legacy — Static Recompilation: Plan

A static recompilation of Mad Doc Software's **Star Trek: Legacy** (Bethesda,
2006), patched to **1.2**, from its shipping Win32 binaries to native C on
pcrecomp's `runtime/win32hle`, drawn through Metal. It follows
[tpw-recomp](../../tpw-recomp) for the shape of the project (disc → game
folder in Python, a lift, a CMake host, a Swift launcher app that builds the
game on the player's Mac) and [redalert2-recomp](../../redalert2-recomp) for
the remaster conventions (presenter, F-key settings, resolutions).

Everything below is from a first survey of the two files (2026-11-06).

**Progress:** Phases 0–3 are done. `tools/install/install.py` builds the
1.2 game folder from the two files in under half a minute, identical to the
survey's tree, then catalogues and lifts all four modules to C with no
errors; the host maps and runs them, and the game runs its main loop on a
Direct3D 9 device that draws nothing, for as long as it is left (three
minutes, 123,035 frames, no fault). What it asks of Direct3D 9 is in
`docs/d3d9-calls.md`: Phase 4's list.

## 1. What we have

| File | Size | MD5 |
|---|---|---|
| `star_trek_legacy.iso` | 2,380,627,968 | `30c793de689aa708e658af4473237054` |
| `star_trek_legacy_1_2_patch.exe` | 5,600,805 | `d48cc0e5aeac1bc51df3d11ac59c22bb` |

Both are on `https://files.cpressland.io/public/stl-recomp/`, so
`install.py --download` works exactly as TPW's does (`STL_DOWNLOAD_BASE`,
resumed with `Range`, checked by MD5).

### 1.1 The disc

A plain ISO 9660 DVD (no Joliet needed, no UDF) holding an **InstallShield 11
MSI** install:

- `Star Trek Legacy.msi`: the tables that name every file;
- `Data1.cab` (400 MB) + `Data11.cab` (1.97 GB): one **spanned** Microsoft
  cabinet set, 14,529 files, all folders **MSZIP**, every file named by its
  MSI file key (`_0612EF1F…`), not its name.

Turning that into the install tree needs no Windows and no installer: read
the MSI's `File`, `Component` and `Directory` tables, extract the cabinet set,
and put each file at `Directory`/`FileName` (the long half of `SHORT|Long`).
Done by hand in this survey (an `olefile`-based MSI table reader and ~30
lines of tree-building): the result is the real install, 3.2 GB:

```
Legacy.exe  (3.5 MB, "43.0.0.0")  binkw32.dll (Bink 1.8.9)  d3dx9_27.dll
msvcr71.dll  msvcp71.dll  NetworkManager.dll  xinput9_1_0.dll  config.cfg
AI/  animations/  campaign/  cursors/  Db/  DbHdrs/  M3D/  misc/  missions/
objectives/  odf/  Shaders/  sol/  sounds/  Sprites/  Textures/  ui/  DirectX/
```

Data: 7,794 `.wav`, 3,307 `.dds`, 1,670 `.odf`, 1,203 `.m3d`, 40 `.ogg`,
37 `.bik` (all `BIKi`, Bink 1), 36 `.fx` HLSL effects, `db.pak`/`dbx.pak`.
`DirectX/` is the redistributable and is skipped.

### 1.2 The 1.2 patch

An InstallShield 11 `setup.exe` whose overlay is the "InstallShield" stream
(8 files, each XOR/nibble-swap encoded with a key from its own name; decoded
in this survey). Inside is `Star Trek Legacy Patch v1.2.msi` (v1.20,
9 Feb 2007) with its cabinet as an embedded stream (**LZX**, 2 MB window).

It is **whole-file replacement**, no binary deltas: 15 files, among them
`Legacy.exe` (MD5 `853616c05b540bf81afbd6e7b738d2f0`, vs 1.0's
`14395a15…`), `NetworkManager.dll`, `Input.map`, 6 shaders and 5 UI XMLs.
Applying it is "copy over the tree". No RTPatch, no Windows applier.

### 1.3 The executable

`Legacy.exe` 1.2: MSVC 7.1 (VS .NET 2003), image base `0x400000`, **no
relocations, no TLS, not large-address-aware**, four ordinary sections
(`.text` 2.7 MB, entropy 6.5). **No DRM**: no SecuROM/SafeDisc/StarForce
sections, no packer, no disc-check strings. ~955k instructions, almost all
x87 floating point (no SSE in the game's own code). Its engine is Mad Doc's
own **Mad3D** (full RTTI: `Mad3D_Device`, `Mad3D_ChunkDatabase`, …), which
`rtti.py` will seed well.

Imports, and what each means for win32hle:

| Import | What it needs | win32hle today |
|---|---|---|
| `KERNEL32` (56), `USER32` (86, many `W`), `GDI32` (25), `ADVAPI32`, `SHELL32`, `ole32`, `COMCTL32`, `comdlg32`, `WINMM` (mmio) | the usual | mostly there; the **wide** USER32 calls (`CreateWindowExW`, `RegisterClassW`, `DefWindowProcW`, …) are new |
| `MSVCR71`, `MSVCP71` (DLLs, not static) | C runtime and C++ standard library | not there → **lift both DLLs** (on the disc) |
| `__CxxFrameHandler`, `_CxxThrowException` | guest C++ exceptions | `eh32` exists for **native32 only** → port to win32hle |
| `d3d9.dll` (by `LoadLibrary` + `Direct3DCreate9`) | **Direct3D 9**, shaders SM 1.1–3.0 | **not there**: win32hle stops at DirectX 6 |
| `d3dx9_27.dll` (23 calls) | `D3DXCreateEffect` (HLSL compiled **at runtime**), DDS/cube/volume textures from memory, matrix maths | → **lift the DLL** |
| `DSOUND` (`DirectSoundCreate`) + `IDirectSound3DBuffer`/`Listener` | DirectSound with **3D buffers** | DirectSound 1, no 3D |
| `DINPUT8` (`DirectInput8Create`, `GUID_SysMouse`) | DirectInput 8 mouse | DirectInput 3/5 |
| `binkw32` (10, incl. `BinkDX9SurfaceType`, `BinkOpenDirectSound`) | Bink 1 movies | `bink.c` on ffmpeg: add the DX9 surface type |
| `NetworkManager.dll` (20 C++ exports) | GameSpy lobby + DirectPlay voice | → stub first; lift later for LAN |
| `WS2_32` (5), `imagehlp` (`UnDecorateSymbolName`) | hostname, crash reports | trivial |

**The shaders.** The 36 `.fxo` files on the disc all start `a3 d7 01 41`
and are big-endian: they are **Xbox 360** compiled effects, left over. The
PC game feeds the `.fx` source (ps_1_1…ps_3_0, vs_1_1…vs_3_0) to
`D3DXCreateEffect`, so **d3dx9_27's HLSL compiler runs at startup**. This is
the strongest argument for lifting `d3dx9_27.dll` rather than reimplementing
it: the compiler, the effect framework, its preshaders and its texture
loaders all come with it, exactly as shipped, and what reaches our code is
plain `IDirect3DDevice9` calls with D3D9 shader bytecode.

`d3dx9_27.dll` (764k instructions, 327 exports, relocatable) has SSE,
SSE2 and **3DNow!** paths chosen by `CPUID`; the host's `CPUID` answers
without 3DNow! (and, at first, without SSE) so the lifter needs nothing new.

## 2. Architecture

```
Star Trek Legacy.app (Swift launcher, Sparkle)          app/
  └─ install.py: ISO + patch → game/ → lift → build     tools/install/
       └─ libstl.dylib, entered at stl_main             src/host/
            ├─ lifted Legacy.exe                        build/gen/legacy/
            ├─ lifted d3dx9_27.dll, msvcr71.dll,        build/gen/<dll>/
            │  msvcp71.dll (later NetworkManager.dll)
            └─ pcrecomp runtime/win32hle
                 ├─ d3d9.c        IDirect3D9 / IDirect3DDevice9 / resources (new)
                 ├─ d3d9_metal.m  the device on Metal (new)
                 ├─ dxso_msl.c    D3D9 shader bytecode → Metal Shading Language (new)
                 ├─ eh32 port, dinput8, dsound 3D, wide USER32 (extensions)
                 └─ screen.c / bink.c / … as TPW uses them
```

Two choices to settle early, with my recommendation:

1. **Lift, don't reimplement, the Microsoft DLLs** (`d3dx9_27`, `msvcr71`,
   `msvcp71`). They ship on the disc, so they come from the player's own
   copy like `Legacy.exe` does, and lifting them gives the HLSL compiler,
   iostream/locale/printf behaviour and STL layouts for free. pcrecomp
   already lifts multi-module games (ut99-recomp: four DLLs rebased;
   civ3's `jgl.dll`). The host overrides a handful of their exports
   (`_CxxThrowException`, `__CxxFrameHandler`, `malloc`/`free` onto
   win32hle's heap if profiling says so). *Fallback*: an HLE msvcrt if
   lifting msvcr71 turns out to fight win32hle's TIB/TLS/heap.
2. **Translate shaders to MSL ourselves, not through SPIR-V.** D3D9 bytecode
   (SM 1.x–3.0) is small, register-based and well documented; a direct
   `dxso → MSL` translator compiled with `MTLDevice newLibraryWithSource`,
   cached by bytecode hash, avoids a SPIRV-Cross dependency in the app's
   toolchain. DXVK's `dxso` and Wine's `vkd3d-shader` are the references for
   semantics (DXVK is zlib-licensed; read, don't copy, Wine's LGPL code).
   Fixed-function D3D9 (the game has `FixedPipelineDiffuse.fxh`, and UI
   quads) is generated as MSL from the state, the way DXVK does.

## 3. Phases

Each phase ends with something that runs, as TPW's bring-up did.

### Phase 0 — scaffold (a day)
- `git init`, MIT licence, `.gitignore` (`work/`, `build*/`, `*.iso`, `*.exe`,
  `game/`), README in TPW's form, `docs/RECON.md` (this survey's facts).
- `CMakeLists.txt`, `build-macos.sh`, `run_lift.py` copied from TPW and
  renamed; `pcrecomp` beside it, on a `stl-local` branch.

### Phase 1 — disc to game folder, in Python (2–3 days)
- `tools/disc/iso.py` (TPW's, as is).
- `tools/disc/msi.py`: MSI table reader (stream-name decoding, string pool,
  column-major tables), on `olefile` (pure Python; added to the app's
  `requirements.txt`).
- `tools/disc/cab.py`: TPW's, extended with **spanned** cabinets
  (`Data1.cab` → `Data11.cab`, files continued across) and **LZX** (pure
  Python; only the patch's 1.4 MB uses it, so speed doesn't matter).
- `tools/disc/isstream.py`: the InstallShield setup.exe stream decoder.
- `install.py` steps: ISO → cabinets → tree from MSI tables → patch MSI →
  overlay → `game/`, checking `Legacy.exe` against the 1.2 MD5. ~3.3 GB.
  APFS clones (`cp -c`) and streaming keep peak disk to about 2× that.

### Phase 2 — catalog and lift, four modules (1–2 weeks)
- `pe_analyze`, `rtti.py`, `disasm32.py` on `Legacy.exe`, `d3dx9_27.dll`,
  `msvcr71.dll`, `msvcp71.dll`; `data_code_ptrs.py` for vtables and jump
  tables; target: no lift errors, coverage in TPW's range (~95%).
- Modules mapped at their preferred bases (`0x400000`, d3dx/msvc*/network
  where they don't collide; rebased by the loader otherwise), bound guest to
  guest as ut99-recomp does.
- **eh32 on win32hle**: `recomp_eh_push/pop` and the dispatcher, with
  `__declspec(thread)` → the runtime's TLS and `windows.h` → `win32hle.h`.
  Mad3D's C++ and D3DX's compiler both throw.

**Done (2026-11-06).** What it came to:

| Module | Base | Functions | Coverage | C++ frames with catches | Lift errors |
|---|---|---|---|---|---|
| `Legacy.exe` | `0x00400000` | 26,033 | 95.0% | 3 (of 184 ebp frames; 1,998 more unwind-only) | 0 |
| `d3dx9_27.dll` | `0x01000000` (moved) | 14,306 | 89.0% | 1 (of 139) | 0 |
| `msvcr71.dll` | `0x7C340000` | 3,375 | 99.8% | 0 | 0 |
| `msvcp71.dll` | `0x7C3A0000` | 3,688 | 99.5% | 133 (of 510, all through `__EH_prolog`) | 0 |

10.7 million lines of C in 121 files, one dispatch table; every function
with a catch block has a landing pad. It all compiles (`CMakeLists.txt`,
an object library until Phase 3's host links it): no errors, 32 minutes on a
10-core M-series Mac at `-O1`, most of it D3DX's 7.5 million lines, so the
app's first build will want D3DX's chunks smaller or at `-O1`. Found on the way:

- `d3dx9_27.dll` wants `0x00400000` too, so it is rebased to `0x01000000`
  first (`tools/gen/rebase.py`, 27,763 relocations), above the game and below
  win32hle's heap. The host must map all four before the first allocation.
- D3DX has no RTTI; its COM vtables come from the disassembler's own scans.
  Runs of code-like pointers in its data that the catalog lacks are numeric
  tables, not vtables (checked: evenly spaced, mid-instruction).
- Legacy opens most EH frames without a frame pointer (`push -1; push
  handler`, no `push ebp`): MSVC does that only for unwind-only frames, and
  all 1,998 are (checked against their FuncInfo), so they need no landing
  pad; the runtime runs their destructors from the guest's own data.
- msvcp71 opens 510 frames through msvcr71's `__EH_prolog`, which
  `lift/eh32.py` did not know: pcrecomp `lift-eh32-eh-prolog`.
- eh32 is pcrecomp `win32hle-eh32`: `hle_eh_throw` for `_CxxThrowException`,
  with msvcr71's own export hooked to it (`run_lift.py` HOOK_EXPORTS) so a
  throw inside the runtime is caught the same way; `eh32_selftest`.
- `disasm32.py` takes 20 minutes on `d3dx9_27.dll` (81 s on Legacy.exe):
  in its second discovery round each of 2,911 new entries re-decodes a large
  body one instruction at a time (15.7 M `disassemble_at` calls for 1,800
  functions). The output is right; the time matters for the app's first
  build. A decode cache in pcrecomp is the fix, minding memory.
- `cpuid` (10 in D3DX, 1 in msvcr71) calls the host's `stl_cpuid`, to be
  written in Phase 3: no 3DNow!, SSE as the lifter allows.

### Phase 3 — boot to a black window (1–2 weeks)
- Host `main.c` on TPW's: drive mapping (`C:` → game folder; the game
  writes saves/settings via `SHGetFolderPathA`, mapped to
  `~/Library/Application Support/Star Trek Legacy`), registry seed
  (Bethesda key, install path, version), permissive bind so every missing
  import names itself.
- Fill the wide USER32/KERNEL32 calls, `SetWindowsHookExW` (accepted, never
  fires), `GetVersionExW`, `SHGetFolderPathA`, `CoCreateGuid`.
- A **d3d9 stub** that creates a device and presents nothing: the game
  reaches its main loop, logs `config.cfg` reads, loads `db.pak`.

**Done (2026-11-07).** `src/host` is the program: four modules mapped
and bound to each other, the DLLs attached, the game entered. What the
runs have turned up so far:

- d3dx9_27 imports **msvcrt.dll**, Windows' own runtime, not msvcr71: its 64
  imports are bound to the lifted msvcr71's exports (`loader.c`).
- The game loads `C:\WINDOWS\SYSTEM32\d3d9.dll` by full path, and win32hle
  had no d3d9, dinput8 or XInput among its system DLLs: pcrecomp
  `win32hle-dx9-system-dlls`. DirectInput 8 itself: `win32hle-dinput8`.
- Its window code is Unicode: `wide.c` answers the W calls on the A ones.
- **Code the catalog missed**, found by running: D3DX keeps its read-only
  data in `.text`, so msvcr71's `_initterm` called 55 unlifted static
  constructors; and the disassembler decoded D3DX's shader-target vtables
  as a function. `data_code_ptrs.py` now reads code sections as data where
  no function is, takes pointer runs anywhere whose targets look like
  function starts, and every hot-patch start (`mov edi, edi` after
  padding): 384 D3DX entries in all.
- Each such fix recompiles D3DX: 33 minutes at first, 17 with its chunks
  split finer, and now only the 64 KB windows that changed (chunks are
  named by address window).
- The game creates its device (1024x768, A8R8G8B8, full screen), D3DX
  compiles the game's effects (81 vertex and 81 pixel shaders, dumped with
  STL_DUMP_SHADERS for Phase 4), and the game sets up DirectSound 3D
  (pcrecomp `win32hle-dsound3d`: listener and 3D buffers, distance rolloff
  and pan in the mixer).
- **The main loop runs**: 3,249 frames presented, tens of thousands of draws,
  before D3DX's compiler `longjmp`ed out of an error and landed nowhere.
  `setjmp`/`longjmp` are now msvcr71 hooks on eh32's landing pads
  (`src/host/setjmp.c`; run_lift.py gives each of D3DX's 7 setjmp calls a
  pad and tells setjmp where it is, as a lifted call pushes no real return
  address).
- A Phase 2 bug, found on the way: pefile's `relocate_image` corrupted
  d3dx9_27's import table in the rebased copy, so every tool saw a DLL with
  no imports (and 166 false code pointers). `rebase.py` now applies the
  relocations itself; only the import table differed, so the lifted code
  was right.

### Phase 4 — Direct3D 9 on Metal (the bulk: 6–10 weeks)
In win32hle, as `d3d9.c` (objects, state, resources in guest memory) and
`d3d9_metal.m` (the GPU side), in steps that each show more of the game:
1. `IDirect3D9`: one adapter, caps of a Shader Model 3 card of 2006
   (GeForce 7800-class), display modes from the screen; `CreateDevice`.
2. Resources: textures (2D/cube/volume, DXT1–5, A8R8G8B8, X8R8G8B8, R5G6B5,
   A16B16G16R16F, R32F, D24S8), mip levels, `LockRect` through a staging copy,
   `D3DPOOL_DEFAULT`/`MANAGED`/`SYSTEMMEM`, render-target textures, vertex and
   index buffers (dynamic with `DISCARD`/`NOOVERWRITE`), vertex declarations
   and FVFs.
3. Draw: `DrawPrimitive`/`DrawIndexedPrimitive`/`*UP`, render state → Metal
   pipeline state (cached), depth-stencil state, samplers, scissor, viewport,
   **clip-space z** (D3D 0..1 matches Metal), the D3D9 half-pixel offset.
4. **`dxso_msl.c`**: vs/ps 1.1, 2.0, 2.b, 3.0 → MSL: registers
   (`r c v t s oPos oD oT oC`), modifiers, `def`/constants, `texld`/`texkill`/
   `texbem` (ps_1_x), control flow (3.0), `dcl` semantics to vertex
   attributes; constants in a buffer per draw. Unit tests: compile every
   shader the game's `.fx` produce (dumped once from a run) through
   `newLibraryWithSource`, offline, in CI.
5. Fixed function generated as MSL from texture-stage and lighting state.
6. Render targets, `StretchRect`, `GetRenderTargetData` (screenshots, the
   shadow map, post-process bloom/glow chain), occlusion queries if used.
7. `Present` onto the screen layer TPW's Metal path already presents through.

Milestones: main menu → a skirmish map with ships → post-process and
shadows → full speed at 1080p/4K on Apple silicon.

**Under way (2026-11-07): the shader translator.** `src/host/dxso.c` turns
D3D9 bytecode (vs/ps 1.1 to 3.0's common instructions, flow control, the
ps_1_x modifiers and texture ops) into MSL with a fixed vertex-to-pixel
interface, so any vertex shader pairs with any pixel shader as in D3D9.
Two tools check it, neither needing the game running:

- `work/dxso_check work/shaders`: every shader the game compiles
  (`STL_DUMP_SHADERS`) through the translator and this Mac's Metal
  compiler. **All 94 compile.**
- `work/dxso_test`: hand-assembled shaders drawn on the GPU into a 1x1
  float target and read back, against D3D9's answers: rcp/rsq/log at 0 and
  of negatives, lit, vs_1_1 expp, mov a0 rounding down with a relative
  constant, m4x4, sge/slt/frc, the oD0 clamp, ps_1_1 _bx2 with _x2, cnd,
  _sat, cmp, lrp, dp2add, nrm, swizzles, texkill. **14 of 14.** (rcp(0) is
  FLT_MAX under Metal's fast maths where the reference rasteriser has
  +inf: kept, as it keeps a later 0 * rcp(0) a 0.)

`tools/dxso/build.sh` builds both. The shaders themselves are the game's
(compiled from its .fx files), so they are not in the repository: dump them
with a run.

**The device on Metal (2026-11-07).** `src/host/d3d9_metal.m` is the GPU
side of `d3d9.c` (whose state it reads at each draw). Headless, with
screenshots (`STL_SHOT`) to look at: the main menu draws whole, and two
scripted clicks (`--click`) later the first campaign mission does, the
NX-01 textured, lit and bump-mapped, planets, the sun's corona and flares,
the nebula, HUD and radar, through the game's own bloom and glow passes.
With a window the picture goes to win32hle's presenter (`screen.c`) by a
readback a frame. What it took beyond the plan:

- D3D9's default render, stage and sampler states (all-zero masked every
  colour write off);
- a shader's length found by walking its tokens (the end token's value in
  D3DX's constant table cut 36 vertex shaders to nothing, and with them
  every model);
- the game pumps its window's messages on a thread of its own, and AppKit
  takes events on the main thread only: SDL is pumped there;
- state blocks that record (D3DX's effects save and restore through them),
  SetFVF and SetVertexDeclaration replacing each other, partial clears.

Since: the intro's movies play (Bink through ffmpeg, when pkg-config
finds it), and the ship takes the throttle (the game reads keys by their
scan codes); eight minutes of flight in the first mission with no fault.
`tools/test/smoke.sh` runs the intro, menu and mission headless and
checks each drew.

Not done: fixed function (the game has drawn none so far), depth bias's
scale, sRGB, table fog, Doppler and sound cones. (The window's copy of each
frame no longer waits for the GPU, and the app carries ffmpeg: 2026-11-08.) Not tried:
combat, skirmish, the other missions, a real mouse and keyboard in the
window.

### Phase 5 — sound, movies, input (2–3 weeks, parallel with 4)
- **DirectSound 3D** in `dsound.c`: listener and buffer positions,
  min/max distance, rolloff, doppler, mixed in software as today.
- **Bink**: `BinkDX9SurfaceType`, `BinkOpenDirectSound`,
  `BinkSetSoundSystem` on the existing ffmpeg `bink.c` (`BIKi` decodes).
- **DirectInput 8** mouse on `dinput.c`; keyboard is messages.
- Gamepad (the game supports one via DirectInput): GameController.framework
  as a DirectInput 8 joystick, later.
- Ogg music is the game's own (lifted) Vorbis; nothing to do.

### Phase 6 — the app (1–2 weeks; most of it is TPW's)

**Started (2026-11-07).** `app/` is TPW's launcher made this game's, and
`install.py` compiles: `app/build-app.sh` makes the app (122 MB), its
setup window draws (`STL_SNAPSHOT`). A release workflow and Sparkle feed
are still to do.

- `app/Launcher` from TPW: `Star Trek Legacy.app`, **Download and Build** /
  **Use Files I Already Have**, progress from `install.py`'s `@step`/
  `@progress`, then **Play** loading `libstl.dylib` at `stl_main` in a child
  process, logs in `~/Library/Logs/Star Trek Legacy/`, Sparkle updates,
  `release.yml` producing the DMG, zip, `install.sh` and appcast.
- Download is 2.2 GB (vs TPW's 630 MB): resumable downloads matter more; free
  space check ~10 GB at peak; the ISO and cabinets are deleted once
  `game/` is made.
- Env vars as TPW's (`STL_HOME`, `STL_ISO`, `STL_PATCH`,
  `STL_DOWNLOAD_BASE`, `STL_BUILD_NOW`, `STL_PLAY_NOW`).

### Phase 7 — remaster and polish (open-ended)
- Any resolution and widescreen (the game supports widescreen modes already;
  check its 16:10/16:9 list), Retina scale, F11 fullscreen, F12 scaling, a
  settings menu as RA2's F10.
- Mouse-confine as TPW; Cmd-Q/Cmd-Tab behaviour.
- Multiplayer: GameSpy is gone. Lift `NetworkManager.dll` and keep LAN/direct
  IP (its DirectPlay voice off); OpenSpy as a later option.
- Linux after macOS, as TPW plans (Vulkan presenter would be a separate
  `d3d9_vulkan` backend behind the same `d3d9_gpu.h`).

## 4. Risks

| Risk | Why | Mitigation |
|---|---|---|
| Direct3D 9 on Metal is the largest new code | win32hle has DirectX 6 only | build it in pcrecomp (`d3d9*.c`, upstream PRs as TPW's), test against the game's own shader set offline, `--renderer` trace mode dumping draws |
| Lifting `d3dx9_27` (764k instr.) | big, SIMD, CPUID-dispatched, its compiler throws C++ exceptions | CPUID without 3DNow!/SSE first; eh32 port; fallback: compile the 36 `.fx` once per build and cache the effect blobs |
| `msvcr71` inside win32hle | the CRT wants TLS, its own heap, `_except_handler3` SEH | eh32 covers C++ frames; SEH `__try` frames (ponytail in eh32) may need adding; fallback HLE CRT |
| Lift build time | ~1.7M instructions across modules vs TPW's ~0.5M | `-O1` for d3dx, per-module objects so a host fix rebuilds nothing lifted; the app's first build is ~20–30 min, said up front |
| Performance | x87 everywhere, a shader compiler in lifted C | the based-memory constant (`RECOMP_MEM_BASE`) as TPW; shader cache on disk across runs |
| Undocumented game formats | `db.pak`, `.m3d`, `.odf` | not needed: the game reads them itself |

## 5. Order of work

1. Phase 0–1: a reproducible `game/` from the two files (`install.py
   --download` end to end). Small, and it unblocks everything.
2. Phase 2: lift all four modules with no errors.
3. Phase 3 with a d3d9 stub: the game's main loop running headless.
4. Phase 4 and 5 in parallel; the app (6) once the menu draws.

## 6. Survey artefacts

`work/` (not to be committed) holds what this survey made:
`work/iso/` (the disc's files), `work/cab/` (the cabinet set, by file key),
`work/v10/` (the 1.0 tree, hard links), `work/patch/` and `work/patchcab/`
(the patch decoded), `work/v12/` (the 1.2 tree, the game folder Phase 1
should reproduce), and `work/survey/` (the prototype MSI reader,
InstallShield-stream decoder and tree builder Phase 1 grows from). `du`
reports 8.6 GB (clones and hard links counted more than once); delete freely.
