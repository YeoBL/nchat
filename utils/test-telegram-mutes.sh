#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/nchat-telegram-mutes.XXXXXX)
trap 'rm -f "$test_dir/test" "$test_dir/app.conf"; rmdir "$test_dir"' EXIT
clang++ -std=c++17 -Ilib/common/src -Ilib/ncutil/src -Ibuild/generated \
  -Ilib/tgchat/ext/td -Ilib/tgchat/ext/td/td/generate/auto \
  utils/test-telegram-mutes.cpp -Lbuild/lib -lncutil -ltdclientshared \
  -Wl,-rpath,"$PWD/build/lib" -o "$test_dir/test"
"$test_dir/test" "$test_dir"
