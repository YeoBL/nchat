#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++14 -pthread -Isrc -Ilib/common/src -Ilib/ncutil/src \
  utils/test-widget-replies.cpp -Lbuild/lib -lncutil \
  -Wl,-rpath,"$PWD/build/lib" -o build/test-widget-replies
build/test-widget-replies
