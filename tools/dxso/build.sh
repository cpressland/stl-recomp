#!/bin/sh
# Build the shader translator's checks into work/:
#   dxso_glsl   GLSL to SPIR-V with glslangValidator (any computer)
#   dxso_check  MSL with this Mac's Metal compiler; dxso_test runs shaders (a Mac)
set -e
cd "$(dirname "$0")/../.."
mkdir -p work
cc -O1 -Wall -Isrc/host src/host/dxso.c tools/dxso/dxso_glsl.c -o work/dxso_glsl
if [ "$(uname)" = Darwin ]; then
    clang -O1 -Wall -fobjc-arc -Isrc/host src/host/dxso.c tools/dxso/dxso_check.m \
        -framework Metal -framework Foundation -o work/dxso_check
    clang -O1 -Wall -fobjc-arc -Isrc/host src/host/dxso.c tools/dxso/dxso_test.m \
        -framework Metal -framework Foundation -o work/dxso_test
fi
