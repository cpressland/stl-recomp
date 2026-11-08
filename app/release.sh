#!/bin/sh
# A release: Star Trek Legacy.app with updates on, a disk image to drag it
# from, a zip for Sparkle and the curl installer, and the appcast that
# points Sparkle at the zip. .github/workflows/release.yml runs it for a tag.
#
#   SPARKLE_PRIVATE_KEY=... VERSION=1.2.0 app/release.sh    # -> build-app/release/
#
# SPARKLE_PRIVATE_KEY: the base64 Ed25519 key whose public half is
# app/sparkle-public-key.txt. REPOSITORY: owner/name on GitHub.
set -e
cd "$(dirname "$0")/.."
: "${VERSION:?give VERSION, e.g. 1.2.0}"
: "${SPARKLE_PRIVATE_KEY:?give SPARKLE_PRIVATE_KEY}"
REPOSITORY=${REPOSITORY:-cpressland/stl-recomp}
OUT=${OUT:-build-app}
R="$OUT/release"
APP="$OUT/Star Trek Legacy.app"
NAME="Star-Trek-Legacy"

SPARKLE_FEED="https://github.com/$REPOSITORY/releases/latest/download/appcast.xml" \
SPARKLE_PUBLIC_KEY=$(cat app/sparkle-public-key.txt) VERSION=$VERSION app/build-app.sh
BUILD=$(plutil -extract CFBundleVersion raw "$APP/Contents/Info.plist")

rm -rf "$R"
mkdir -p "$R"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$R/$NAME-$VERSION.zip"
stage=$(mktemp -d)
ditto "$APP" "$stage/Star Trek Legacy.app"
ln -s /Applications "$stage/Applications"
hdiutil create -quiet -volname "Star Trek Legacy" -srcfolder "$stage" -fs APFS -format ULMO "$R/$NAME-$VERSION.dmg"
rm -rf "$stage"
# Names that stay put, for releases/latest/download/... links.
cp "$R/$NAME-$VERSION.zip" "$R/$NAME.zip"
cp "$R/$NAME-$VERSION.dmg" "$R/$NAME.dmg"
cp app/install.sh "$R/install.sh"

# The appcast: the one item this release is.
sig=$(printf '%s' "$SPARKLE_PRIVATE_KEY" | "$OUT/toolchain/sparkle/bin/sign_update" --ed-key-file - "$R/$NAME-$VERSION.zip")
cat > "$R/appcast.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Star Trek Legacy</title>
    <item>
      <title>$VERSION</title>
      <link>https://github.com/$REPOSITORY/releases/tag/v$VERSION</link>
      <sparkle:releaseNotesLink>https://github.com/$REPOSITORY/releases/tag/v$VERSION</sparkle:releaseNotesLink>
      <pubDate>$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')</pubDate>
      <sparkle:version>$BUILD</sparkle:version>
      <sparkle:shortVersionString>$VERSION</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>26.0</sparkle:minimumSystemVersion>
      <enclosure url="https://github.com/$REPOSITORY/releases/download/v$VERSION/$NAME-$VERSION.zip" type="application/octet-stream" $sig/>
    </item>
  </channel>
</rss>
XML
ls -la "$R"
