#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
/usr/bin/python3 utils/test-kitty-badge-auth.py
/usr/bin/python3 utils/test-telegram-archive.py
notification_test_dir=$(mktemp -d /tmp/nchat-notifications.XXXXXX)
trap 'rm -f "$notification_test_dir/state" "$notification_test_dir/forum" "$notification_test_dir/membership" "$notification_test_dir/cache" "$notification_test_dir/badge" "$notification_test_dir/handlers" "$notification_test_dir/ui.conf" "$notification_test_dir/app.conf" "$notification_test_dir/color.conf"; rmdir "$notification_test_dir"' EXIT
clang++ -std=c++17 -Ilib/tgchat/ext/td/td/generate/auto -Ilib/tgchat/ext/td \
  utils/test-telegram-membership.cpp build/lib/libtdapi.a build/lib/libtdutils.a \
  /opt/homebrew/opt/openssl@3/lib/libcrypto.dylib -o "$notification_test_dir/membership"
"$notification_test_dir/membership"
clang++ -std=c++14 -Wall -Wextra -pedantic utils/test-forum-reads.cpp -o "$notification_test_dir/forum"
"$notification_test_dir/forum"
clang++ -std=c++14 -Wall -Wextra -pedantic utils/test-notification-state.cpp -o "$notification_test_dir/state"
"$notification_test_dir/state"
clang++ -std=c++14 -Ilib/common/src -Ilib/ncutil/src -Ilib/ncutil/ext/sqlite_modern_cpp/hdr \
  -I/opt/homebrew/opt/sqlite/include utils/test-notification-cache.cpp \
  -Lbuild/lib -lncutil /opt/homebrew/opt/sqlite/lib/libsqlite3.dylib \
  -Wl,-rpath,"$PWD/build/lib" -o "$notification_test_dir/cache"
"$notification_test_dir/cache"
# Exercise compiled production message handlers; only terminal drawing is stubbed.
objects=()
for object in build/CMakeFiles/nchat.dir/src/*.cpp.o; do
  case "$object" in */main.cpp.o|*/uiview.cpp.o) continue ;; esac
  objects+=("$object")
done
clang++ -std=c++14 -Ilib/common/src -Ilib/ncutil/src -Ibuild/generated \
  -Ilib/ncutil/ext/sqlite_modern_cpp/hdr -I/opt/homebrew/opt/sqlite/include \
  -I/opt/homebrew/opt/ncurses/include utils/test-notification-handlers.cpp "${objects[@]}" \
  -Lbuild/lib -lncutil -lpthread /opt/homebrew/opt/ncurses/lib/libncursesw.dylib \
  /opt/homebrew/opt/ncurses/lib/libformw.dylib -framework CoreFoundation \
  /opt/homebrew/lib/libmagic.dylib /opt/homebrew/opt/sqlite/lib/libsqlite3.dylib \
  build/lib/libclip.a -framework Cocoa build/lib/libqrcodegen.a build/lib/libstb.a \
  -Wl,-rpath,"$PWD/build/lib" -o "$notification_test_dir/handlers"
if [[ "${1:-}" == "--live-badge" ]]; then
  "$notification_test_dir/handlers" "$notification_test_dir" --live-badge
else
  "$notification_test_dir/handlers" "$notification_test_dir"
fi
if [[ "${1:-}" == "--live-badge" ]]; then
  clang++ -std=c++14 utils/test-terminal-badge.cpp -o "$notification_test_dir/badge"
  # Explicit opt-in: briefly changes Kitty's badge, never its focus or tabs.
  for ((cycle = 1; cycle <= 20; ++cycle)); do
    "$notification_test_dir/badge" set
    "$notification_test_dir/badge"
  done
  printf 'Kitty NSDockTile: 20 acknowledged set/clear cycles passed (not LaunchServices metadata)\n'
fi
