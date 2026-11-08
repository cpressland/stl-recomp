#!/bin/sh
# Star Trek Legacy.app: nothing of the game, everything to build it and run
# it. The launcher (app/Launcher, Swift), the tools it builds the game with
# (app/toolchain.sh: Python with the lift's packages, CMake and Ninja), the
# sources it builds (tools/install/install.py --export-sources), SDL, Sparkle,
# the icon. The app builds the game on each Mac into Application Support.
#
#   app/build-app.sh                 # -> build-app/Star Trek Legacy.app
#   VERSION=1.2.0 app/build-app.sh   # the version it shows (default: git describe)
#
# SPARKLE_FEED and SPARKLE_PUBLIC_KEY turn updates on (app/release.sh sets
# them); without them the app never checks. Apple Silicon and macOS 26 on;
# signed ad hoc, not notarized (README).
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build-app}
APP="$OUT/Star Trek Legacy.app"
C="$APP/Contents"
T="$OUT/toolchain"
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null | sed 's/^v//')}
BUILD=${BUILD:-$(git rev-list --count HEAD 2>/dev/null || echo 1)}

[ -x "$T/python/bin/python3" ] && [ -f "$T/ffmpeg/lib/libavcodec.a" ] || app/toolchain.sh

rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Resources" "$C/Frameworks"
swiftc -O -target arm64-apple-macos26.0 -F "$T/Frameworks" -framework Sparkle \
  -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
  app/Launcher/*.swift -o "$C/MacOS/Star Trek Legacy"
for fw in SDL2 SDL2_ttf Sparkle; do ditto "$T/Frameworks/$fw.framework" "$C/Frameworks/$fw.framework"; done
ditto "$T/python" "$C/Resources/toolchain/python"
ditto "$T/ffmpeg" "$C/Resources/toolchain/ffmpeg"
"$T/python/bin/python3" -I tools/install/install.py --export-sources "$C/Resources/source" >/dev/null

sed -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD/" app/Info.plist > "$C/Info.plist"
if [ -n "$SPARKLE_FEED" ] && [ -n "$SPARKLE_PUBLIC_KEY" ]; then
  plutil -insert SUFeedURL -string "$SPARKLE_FEED" "$C/Info.plist"
  plutil -insert SUPublicEDKey -string "$SPARKLE_PUBLIC_KEY" "$C/Info.plist"
  plutil -insert SUEnableAutomaticChecks -bool YES "$C/Info.plist"
  plutil -insert SUAutomaticallyUpdate -bool NO "$C/Info.plist"    # Sparkle asks first
fi
swift app/icon.swift "$OUT/AppIcon.iconset" >/dev/null
iconutil -c icns "$OUT/AppIcon.iconset" -o "$C/Resources/AppIcon.icns"
rm -rf "$OUT/AppIcon.iconset"

# Ad hoc, innermost first: every program and library the toolchain has, the
# frameworks (Sparkle's helpers within), then the app.
find "$C/Resources/toolchain" -type f \( -perm -u+x -o -name '*.so' -o -name '*.dylib' \) -print0 |
  xargs -0 file | grep Mach-O | cut -d: -f1 | while read -r f; do codesign --force --sign - "$f"; done
for fw in SDL2 SDL2_ttf Sparkle; do codesign --force --deep --sign - "$C/Frameworks/$fw.framework"; done
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP"
du -sh "$APP" | sed "s|\$| ($VERSION)|"
