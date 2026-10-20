#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_dir="${1:-$source_dir/../build/nchat-record.app}"
/usr/bin/install -d "$app_dir/Contents/MacOS"
/bin/cp "$source_dir/nchat-record-Info.plist" "$app_dir/Contents/Info.plist"
/usr/bin/swiftc -O -framework AppKit -framework AVFoundation -framework AVKit \
  "$source_dir/nchat-record.swift" -o "$app_dir/Contents/MacOS/nchat-record"
/usr/bin/codesign --force --sign - "$app_dir"
printf '%s\n' "$app_dir/Contents/MacOS/nchat-record"
