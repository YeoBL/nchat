#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Build ncutil after protocol/serialization changes before running this check.
test_dir=$(mktemp -d /tmp/nchat-telegram-view-once.XXXXXX)
trap 'rm -f "$test_dir/test" "$test_dir/app.conf" "$test_dir/synthetic-photo.jpg" \
  "$test_dir/history/version" "$test_dir/history/Telegram_view_once_fixture/version" \
  "$test_dir/history/Telegram_view_once_fixture/db.sqlite" \
  "$test_dir/history/Telegram_view_once_fixture/db.sqlite-journal"; \
  rmdir "$test_dir/history/Telegram_view_once_fixture" "$test_dir/history" "$test_dir" 2>/dev/null || true' EXIT
clang++ -std=c++17 -Ilib/common/src -Ilib/ncutil/src -Ibuild/generated \
  -Ilib/tgchat/ext/td -Ilib/tgchat/ext/td/td/generate/auto \
  utils/test-telegram-view-once.cpp -Lbuild/lib -lncutil -ltdclientshared \
  -Wl,-rpath,"$PWD/build/lib" -o "$test_dir/test"
"$test_dir/test" "$test_dir"
