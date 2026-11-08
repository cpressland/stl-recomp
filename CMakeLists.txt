# =============================================================================
# Star Trek: Legacy - static recompilation
#
#   python3 -I tools/gen/analyse.py GAME work/analysis    # the lift's inputs
#   python3 run_lift.py                                    # -> work/gen/
#   cmake -S . -B build-macos -G Ninja && cmake --build build-macos
#
# The lifted C of all four modules, pcrecomp's runtime/win32hle and the host
# (src/host) make `stl`, the program, and libstl.dylib, the same game for the
# app to load, entered at stl_main.
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(stl C)
if(APPLE)
  enable_language(OBJC)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  message(FATAL_ERROR "macOS first, Linux next")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "a 64-bit host: the guest's 4 GB is one block of its memory (based)")
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/work/gen" CACHE PATH "the lifted C (run_lift.py --out)")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- run python3 run_lift.py first")
endif()

# A fixed address for the guest's 4 GB lets the lifted code add a constant,
# not a global it reloads after every store (tpw-recomp's choice: 16 TB up is
# free on a Mac, arm64 and x86-64).
if(APPLE)
  set(STL_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
else()
  set(STL_MEM_BASE "" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
endif()

add_library(stl_gen OBJECT ${GEN_SOURCES})
set_target_properties(stl_gen PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(stl_gen PRIVATE ${GEN_DIR} ${PCRECOMP}/runtime/win32hle ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(stl_gen PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS)
if(STL_MEM_BASE)
  target_compile_definitions(stl_gen PRIVATE RECOMP_MEM_BASE=${STL_MEM_BASE})
endif()
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
target_compile_options(stl_gen PRIVATE -w -fno-strict-aliasing -fwrapv)
# D3DX is its HLSL compiler and effect loader, run at start-up: -O1 halves
# its build (7.5 million of the 11 million lines) for no frame-rate cost.
set(D3DX_OPT "-O1" CACHE STRING "optimisation for D3DX's generated C")
foreach(f ${GEN_SOURCES})
  if(f MATCHES "recomp_d3dx9_")
    set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "${D3DX_OPT}")
  else()
    set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "${GEN_OPT}")
  endif()
endforeach()

# ---- the host ---------------------------------------------------------------
# SDL2 and SDL2_ttf from pkg-config (Homebrew), or as the frameworks Star
# Trek Legacy.app carries (app/toolchain.sh): STL_SDL_FRAMEWORKS names their
# folder. The sources include <SDL2/SDL.h>, so both frameworks' headers are
# gathered into one SDL2 folder (tpw-recomp's way).
set(STL_SDL_FRAMEWORKS "" CACHE PATH "a folder with SDL2.framework and SDL2_ttf.framework (empty: pkg-config)")
add_library(stl_sdl INTERFACE)
if(STL_SDL_FRAMEWORKS)
  set(SDL_INCLUDE ${CMAKE_BINARY_DIR}/sdl-include)
  file(REMOVE_RECURSE ${SDL_INCLUDE})
  file(GLOB SDL_HEADERS ${STL_SDL_FRAMEWORKS}/SDL2.framework/Headers/*.h ${STL_SDL_FRAMEWORKS}/SDL2_ttf.framework/Headers/*.h)
  file(COPY ${SDL_HEADERS} DESTINATION ${SDL_INCLUDE}/SDL2)
  target_include_directories(stl_sdl INTERFACE ${SDL_INCLUDE} ${SDL_INCLUDE}/SDL2)
  find_library(STL_SDL2 SDL2 PATHS ${STL_SDL_FRAMEWORKS} NO_DEFAULT_PATH REQUIRED)
  find_library(STL_SDL2_TTF SDL2_ttf PATHS ${STL_SDL_FRAMEWORKS} NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(stl_sdl INTERFACE ${STL_SDL2} ${STL_SDL2_TTF})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
  target_link_libraries(stl_sdl INTERFACE PkgConfig::SDL2 PkgConfig::SDL2_TTF)
endif()
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/host/*.c")
list(FILTER HOST_SOURCES EXCLUDE REGEX "/exe\\.c$")

add_library(stl_obj OBJECT ${HOST_SOURCES} ${HLE_SOURCES})
set_target_properties(stl_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(stl_obj PRIVATE src/host ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(stl_obj PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS)
if(STL_MEM_BASE)
  target_compile_definitions(stl_obj PRIVATE RECOMP_MEM_BASE=${STL_MEM_BASE})
endif()
target_compile_options(stl_obj PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(stl_obj PUBLIC stl_sdl pthread m)
# Movies: pcrecomp's bink.c decodes Bink through ffmpeg. STL_FFMPEG names
# a static build with Bink alone (include/, lib/: what Star Trek Legacy.app
# carries, app/toolchain.sh); without it, pkg-config's (Homebrew's ffmpeg)
# when not building on the app's frameworks. With neither, the movies are
# skipped, as the game's own code allows.
set(STL_FFMPEG "" CACHE PATH "a static ffmpeg: include/ and lib/ (empty: pkg-config's, if any)")
if(STL_FFMPEG)
  foreach(lib avformat avcodec swresample avutil)
    if(NOT EXISTS ${STL_FFMPEG}/lib/lib${lib}.a)
      message(FATAL_ERROR "STL_FFMPEG: no lib${lib}.a in ${STL_FFMPEG}/lib")
    endif()
    list(APPEND STL_FFMPEG_LIBS ${STL_FFMPEG}/lib/lib${lib}.a)
  endforeach()
  target_include_directories(stl_obj SYSTEM PRIVATE ${STL_FFMPEG}/include)
  target_compile_definitions(stl_obj PRIVATE HLE_WITH_FFMPEG)
  target_link_libraries(stl_obj PUBLIC ${STL_FFMPEG_LIBS} m
                        "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreMedia")
  message(STATUS "movies: ffmpeg from ${STL_FFMPEG}")
elseif(NOT STL_SDL_FRAMEWORKS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
  if(FFMPEG_FOUND)
    target_compile_definitions(stl_obj PRIVATE HLE_WITH_FFMPEG)
    target_link_libraries(stl_obj PUBLIC PkgConfig::FFMPEG)
    message(STATUS "movies: ffmpeg ${FFMPEG_libavcodec_VERSION}")
  endif()
endif()
if(APPLE)
  target_sources(stl_obj PRIVATE ${HLE}/d3d_metal.m src/host/d3d9_metal.m)
  set_source_files_properties(${HLE}/d3d_metal.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
  set_source_files_properties(src/host/d3d9_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(stl_obj PUBLIC "-framework Metal" "-framework Foundation")
endif()

add_executable(stl src/host/exe.c)
target_link_libraries(stl PRIVATE stl_obj stl_gen)
if(STL_SDL_FRAMEWORKS)
  set_target_properties(stl PROPERTIES BUILD_RPATH ${STL_SDL_FRAMEWORKS})
endif()
add_library(stl_game SHARED)
set_target_properties(stl_game PROPERTIES OUTPUT_NAME stl)
target_link_libraries(stl_game PRIVATE stl_obj stl_gen)
