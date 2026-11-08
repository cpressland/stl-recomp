#!/bin/sh
# The build tools Star Trek Legacy.app carries, so a Mac needs only Xcode's
# command line tools to build the game: a standalone Python with the lift's
# packages and CMake and Ninja, SDL2 and SDL2_ttf, Sparkle for updates, and
# ffmpeg's Bink decoders for the movies (built here, static, Bink alone).
# Every download is pinned by its SHA-256; arm64 only, and pruned.
#
#   app/toolchain.sh        # -> build-app/toolchain/{python,Frameworks,sparkle,ffmpeg}
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build-app}
CACHE="$OUT/cache"
T="$OUT/toolchain"

PY=cpython-3.13.18+20261026-aarch64-apple-darwin-install_only.tar.gz
fetch() {   # url sha256 -> $CACHE/<name>
  f="$CACHE/$(basename "$1")"
  [ -f "$f" ] || curl -fsSL --create-dirs -o "$f" "$1"
  echo "$2  $f" | shasum -a 256 -c --status || { echo "checksum mismatch: $f" >&2; exit 1; }
}
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/$PY" \
  e5af2fde6a937a0e0c795a0c595c4efb70578d57463a2657c819992b9b9acbac
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.dmg \
  4a7ac31640d70214e848f994be8a12849c0f97918a7e6c2e27a40036166d1a7f
fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.dmg \
  677404e23a1a5f6efcbe64697471c5161283e4af5a6403dcb92235d7d83805cd
fetch https://github.com/sparkle-project/Sparkle/releases/download/2.10.1/Sparkle-2.10.1.tar.xz \
  1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
fetch https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz \
  733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

rm -rf "$T"
mkdir -p "$T/Frameworks" "$T/sparkle"

# Python, its packages, then what the build never runs.
tar -xzf "$CACHE/$PY" -C "$T"
"$T/python/bin/python3" -I -m pip install -q --disable-pip-version-check --no-compile \
  --only-binary :all: -r app/requirements.txt
P="$T/python/lib/python3.13"
CM="$P/site-packages/cmake/data"
rm -rf "$CM/doc" "$CM/bin/ccmake" "$CM/bin/cpack" "$CM/bin/ctest" \
  "$P/idlelib" "$P/tkinter" "$P/turtledemo" "$P/ensurepip" "$P/test" "$P/lib2to3" \
  "$P/site-packages/pip" "$P"/site-packages/pip-*.dist-info \
  "$T/python/lib/tcl"* "$T/python/lib/tk"* "$T/python/lib/libtcl"* "$T/python/lib/libtk"* \
  "$T/python/lib/itcl"* "$T/python/lib/thread"* "$T/python/share" \
  "$T"/python/bin/idle* "$T"/python/bin/pip* "$T"/python/bin/pydoc* \
  "$T"/python/bin/ccmake "$T"/python/bin/cpack "$T"/python/bin/ctest
rm -f "$P"/lib-dynload/_tkinter*.so "$T/python/bin/cmake"   # pip's launcher names this Mac's path
find "$T/python" -name '*.a' -delete
find "$T/python" -name __pycache__ -type d -prune -exec rm -rf {} +

# The frameworks, from their disk images.
for name in SDL2-2.32.10:SDL2 SDL2_ttf-2.24.0:SDL2_ttf; do
  dmg=${name%%:*}; fw=${name##*:}
  mnt=$(mktemp -d)
  hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mnt" "$CACHE/$dmg.dmg"
  ditto "$mnt/$fw.framework" "$T/Frameworks/$fw.framework"
  hdiutil detach -quiet "$mnt"
done
tar -xJf "$CACHE/Sparkle-2.10.1.tar.xz" -C "$T/sparkle"
ditto "$T/sparkle/Sparkle.framework" "$T/Frameworks/Sparkle.framework"

# ffmpeg, for the game's Bink movies (pcrecomp's bink.c): the Bink demuxer
# and its video and audio decoders, static, nothing else (8 MB, half a
# minute). The game links these on the player's Mac (install.py --ffmpeg).
FF="$OUT/ffmpeg-build"
rm -rf "$FF" && mkdir -p "$FF"
tar -xJf "$CACHE/ffmpeg-7.1.1.tar.xz" -C "$FF"
( cd "$FF/ffmpeg-7.1.1" &&
  ./configure --prefix="$PWD/../out" --arch=arm64 --target-os=darwin --extra-cflags=-mmacosx-version-min=26.0 \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-everything --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
    --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct >/dev/null &&
  make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null )
mkdir -p "$T/ffmpeg"
ditto "$FF/out/include" "$T/ffmpeg/include"
ditto "$FF/out/lib" "$T/ffmpeg/lib" && rm -rf "$T/ffmpeg/lib/pkgconfig"
rm -rf "$FF"

# arm64 only: every universal Mach-O thinned.
find "$T/python" "$T/Frameworks" -type f -perm -u+x -o -type f -name '*.so' -o -type f -name '*.dylib' | while read -r f; do
  if lipo -archs "$f" 2>/dev/null | grep -q x86_64 && lipo -archs "$f" | grep -q arm64; then
    lipo -thin arm64 "$f" -output "$f.thin" && mv "$f.thin" "$f"
  fi
done
find "$T/Frameworks" -type f -path '*/Versions/A/*' | while read -r f; do
  if lipo -archs "$f" 2>/dev/null | grep -q x86_64 && lipo -archs "$f" | grep -q arm64; then
    lipo -thin arm64 "$f" -output "$f.thin" && mv "$f.thin" "$f"
  fi
done
du -sh "$T/python" "$T/Frameworks" "$T/ffmpeg"
