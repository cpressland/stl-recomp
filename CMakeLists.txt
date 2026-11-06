# =============================================================================
# Star Trek: Legacy - static recompilation
#
#   python3 -I tools/gen/analyse.py GAME work/analysis    # the lift's inputs
#   python3 run_lift.py                                    # -> work/gen/
#   cmake -S . -B build-macos -G Ninja && cmake --build build-macos
#
# Phase 2 (docs/PLAN.md): the lifted C of all four modules compiles, as an
# object library, against pcrecomp's runtime headers. The host that links it
# with runtime/win32hle into a program comes in Phase 3.
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(stl C)

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
target_compile_options(stl_gen PRIVATE -w ${GEN_OPT} -fno-strict-aliasing -fwrapv)
