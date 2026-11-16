# Linux, and the Steam Deck (begun 2026-11-16)

The port follows tpw-recomp's: the same game folder, lift and build, made
on the player's computer from the disc image and the patch; Direct3D drawn
through Vulkan where the Mac draws it through Metal; a Flatpak (and later
an AppImage) that carries the build's tools and none of the game.

## What tpw-recomp did, and what carries over

| tpw-recomp | Here |
|---|---|
| install.py on Linux: `$XDG_DATA_HOME/theme-park-world`, `libtpw.so`, any C compiler on the PATH, files named as the disc spells them | the same changes to `tools/install/install.py` |
| Direct3D (DirectX 6) through Vulkan: pcrecomp's `d3d_vulkan.c`, Vulkan 1.3 with dynamic rendering, no swapchain, the loader opened at run time, frames copied down and shown by win32hle's presenter | the same shape, for Direct3D 9: `src/host/d3d9_vulkan.c` beside `d3d9_metal.m`, behind the same `gpu_*` calls (`d3d9_int.h`) |
| its shaders: one fixed fragment shader, GLSL compiled to SPIR-V when pcrecomp is built (`vulkan/spv.sh`) | **different**: this game's 97 shaders are its own D3D9 bytecode, translated as it runs (`dxso.c`, to MSL today). Linux needs GLSL from `dxso.c` too, compiled to SPIR-V at run time by shaderc (`libshaderc_shared.so.1`, opened at run time as the Vulkan loader is: the freedesktop SDK 26.08 has it) |
| a Flatpak on the freedesktop SDK (the player's computer compiles the game): SDL2 through sdl2-compat, SDL2_ttf built, the lift's Python packages, `launcher.c` (SDL, made for a controller), CI on a tag, published through cpressland/flatpak | the same, renamed; ffmpeg's Bink decoders added as a module |
| an AppImage carrying zig, Python, CMake and SDL | later, after the Flatpak |
| Ctrl+Super lets go of the mouse | comes with win32hle |

## Milestones

1. **Build and run headless on the Deck.** The host and win32hle built with
   gcc in the freedesktop SDK (TPW's Flatpak on the Deck is a ready
   sandbox for it), no GPU backend yet (`gpu_*` do nothing), and the game
   run to its menu and a mission headless: the lift, the loader, threads,
   files by any case, sound off.
   **Done (2026-11-16):** the Deck's build took 65 minutes (3 files at a
   time, the least memory free 2.9 GB of 14.8), and the smoke test's run
   reached the watchdog with no fault, all 8 inputs taken and the
   campaign's first mission script (`STL12S.dsl`) loaded. The Deck's own
   controls were found as a controller.
2. **dxso to GLSL.** A second backend in `dxso.c`, the same fixed varyings
   and buffers; the shader corpus (`STL_DUMP_SHADERS`) all compiling to
   SPIR-V with `glslangValidator` (`tools/dxso`), on the Deck or a Mac.
   **Done (2026-11-16):** `dxso_options.lang = DXSO_GLSL`. The instruction
   walk is shared: the body is written in MSL's spelling and renamed
   (`float4` `vec4`, `rsqrt` `inversesqrt`, ...), with GLSL's own forms
   where it differs (vector comparisons, `select` as `mix`, sampling,
   `discard`, the bools as `uvec4[4]`). The bindings are in `dxso.h`.
   `work/dxso_glsl` compiles all 97 dumped shaders (vs_1_1, vs_2_0, ps_1_1,
   ps_1_2, ps_2_0) to SPIR-V for Vulkan 1.3, here with spirv-val and on the
   Deck with the SDK's glslang; `-depth`, `-cube` and `-3d` force the
   texture kinds a dump cannot show (shadow comparisons, ps_1_x's cube and
   volume stages), and they compile too. The Vulkan side must flip y with a
   negative viewport height and choose the front face to match.
3. **`d3d9_vulkan.c`.** Device, render passes as dynamic rendering, the
   ring of dynamic data, textures and format conversions, pipelines and
   their caches, samplers, clears, StretchRect, readback and shots, and
   the window's frame copied down as the Metal one is. Then the game on
   the Deck's screen, and the smoke test there.
4. **The Flatpak**, from tpw-recomp's manifest and launcher, and its CI.
5. **The AppImage**, if wanted.

## The Deck

SteamOS on x86-64, 8 cores, 14 GB, RADV (Vulkan 1.4). No compiler of its
own: the freedesktop SDK 26.08 (installed, for Theme Park World's Flatpak)
has gcc 16, CMake, Ninja, Python, Vulkan's headers, glslang and shaderc.

gcc's memory is the limit. It ignores clang's `optnone`, so the very long
lifted functions are marked `RECOMP_BIG_FN` (`optimize("O0")` on gcc), and
it holds a whole file as it compiles, so the lift writes no file over 8 MB
(`MAX_FILE_BYTES`; one D3DX file was 63 MB). install.py runs one compile
per 4 GB of memory (`--jobs` to choose). SteamOS's earlyoom kills the
biggest process when memory runs low, and once that was the Steam client.
