#!/bin/sh
# Packs dist/ascii-patrol-<target>.tar.gz (.zip for Windows) for av: the
# binary at the top, with the licences, README.md and PATCHES.md.
#
#   asciiverse/package.sh x86_64-unknown-linux-gnu
set -eu
cd "$(dirname "$0")/.."
target="$1"
case "$target" in *windows*) exe=.exe ;; *) exe= ;; esac
stage=stage/ascii-patrol
rm -rf "$stage" && mkdir -p "$stage" dist
cp "asciipat$exe" LICENSE README.md PATCHES.md "$stage/"
cp miniaudio/LICENSE "$stage/MINIAUDIO-LICENSE"
if [ -n "$exe" ]; then
  (cd "$stage" && zip -qr "../../dist/ascii-patrol-$target.zip" .)
else
  tar -czf "dist/ascii-patrol-$target.tar.gz" -C "$stage" .
fi
ls dist
