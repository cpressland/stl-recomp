#!/bin/sh
# Build work/dxso_check: the shader translator and this Mac's Metal compiler.
set -e
cd "$(dirname "$0")/../.."
mkdir -p work
clang -O1 -Wall -fobjc-arc -Isrc/host src/host/dxso.c tools/dxso/dxso_check.m \
    -framework Metal -framework Foundation -o work/dxso_check
clang -O1 -Wall -fobjc-arc -Isrc/host src/host/dxso.c tools/dxso/dxso_test.m \
    -framework Metal -framework Foundation -o work/dxso_test
