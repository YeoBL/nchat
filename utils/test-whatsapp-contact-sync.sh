#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/nchat-contact-sync.XXXXXX)
trap 'rm -f "$test_dir/test"; rmdir "$test_dir"' EXIT
clang++ -std=c++14 -Ilib/common/src -Ilib/ncutil/src -Ilib/wmchat/src \
  -Ilib/ncutil/ext/sqlite_modern_cpp/hdr -I/opt/homebrew/opt/sqlite/include \
  utils/test-whatsapp-contact-sync.cpp -Lbuild/lib -lncutil \
  /opt/homebrew/opt/sqlite/lib/libsqlite3.dylib \
  -Wl,-rpath,"$PWD/build/lib" -o "$test_dir/test"
"$test_dir/test"
