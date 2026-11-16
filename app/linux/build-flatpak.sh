#!/bin/bash
# Build the Flatpak for this machine's architecture into an OSTree repository.
#
#   app/linux/build-flatpak.sh [--version 1.2.0] OUT
#
# OUT/repo gets the commit (branch stable), unsigned: cpressland/flatpak's
# publish.sh merges the architectures (and other games), signs and makes the
# repository players add. The app is built from this repository's HEAD,
# checked-in files only (never the game), and pcrecomp at app/pcrecomp.lock.
# Needs flatpak-builder and the Flathub remote (for org.freedesktop.Sdk).
# FLATPAK_BUILDER_ARGS: extra arguments (--disable-rofiles-fuse in a
# container without FUSE).
set -euo pipefail

version=
if [ "${1:-}" = --version ]; then version=$2; shift 2; fi
[ $# -eq 1 ] || { sed -n '2,11p' "$0"; exit 2; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
commit=$(git -C "$repo" rev-parse HEAD)
[ -n "$version" ] || version=$(git -C "$repo" describe --tags --match 'v[0-9]*' --always | sed 's/^v//')
if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
  echo "note: uncommitted changes are not in the build (it takes $commit)" >&2
fi
pc_repository=$(sed -n 's/^repository=//p' "$repo/app/pcrecomp.lock")
pc_ref=$(sed -n 's/^ref=//p' "$repo/app/pcrecomp.lock")

manifest=$out/io.cpressland.StarTrekLegacy.yml
sed -e "s|@REPO@|$repo|" -e "s|@COMMIT@|$commit|" -e "s|@VERSION@|$version|g" -e "s|@DATE@|$(date -u +%F)|" \
    -e "s|@PCRECOMP_REPOSITORY@|$pc_repository|" -e "s|@PCRECOMP_REF@|$pc_ref|" \
    "$here/io.cpressland.StarTrekLegacy.yml.in" > "$manifest"

echo "==> Star Trek: Legacy $version ($commit), pcrecomp $pc_ref, $(uname -m)"
# shellcheck disable=SC2086
flatpak-builder --force-clean --default-branch=stable --install-deps-from=flathub \
  --state-dir="$out/.flatpak-builder" --repo="$out/repo" ${FLATPAK_BUILDER_ARGS:-} \
  "$out/build" "$manifest"
echo "==> $out/repo"
