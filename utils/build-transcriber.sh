#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir="${1:-$source_dir/../build/nchat-transcribe.app}"
/usr/bin/install -d "$app_dir/Contents/MacOS"
/bin/cp "$source_dir/nchat-transcribe-Info.plist" "$app_dir/Contents/Info.plist"
/usr/bin/swiftc -O -framework Speech "$source_dir/nchat-transcribe.swift" \
  -o "$app_dir/Contents/MacOS/nchat-transcribe"
/usr/bin/codesign --force --sign - "$app_dir"
printf '%s\n' "$app_dir/Contents/MacOS/nchat-transcribe"
