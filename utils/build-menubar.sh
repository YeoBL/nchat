#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
app="build/nchat Unread.app"
mkdir -p "$app/Contents/MacOS"
xcrun swiftc utils/nchat-menubar.swift utils/nchat-widget-reply.swift -o "$app/Contents/MacOS/nchat-menubar" \
  -framework AppKit -framework ImageIO -lsqlite3
install -m 644 utils/nchat-menubar-Info.plist "$app/Contents/Info.plist"
codesign --force --sign - "$app"
# Retain the convenient command-line test entry point used by older scripts.
ln -sf 'nchat Unread.app/Contents/MacOS/nchat-menubar' build/nchat-menubar
"$app/Contents/MacOS/nchat-menubar" --self-test
"$app/Contents/MacOS/nchat-menubar" --test-popover-layout
printf 'Built %s\n' "$PWD/$app"
