#!/bin/sh
# Reuse CMake's actual includes and link dependencies, keeping assertions enabled.
set -eu
cd "$(dirname "$0")/.."
source_dir=$(pwd -P)
build_dir=${1:-build}
cmake --build "$build_dir" --target nchat
cd "$build_dir"
compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' CMakeCache.txt)
test -n "$compiler"
defines=$(sed -n 's/^CXX_DEFINES = //p' CMakeFiles/nchat.dir/flags.make)
includes=$(sed -n 's/^CXX_INCLUDES = //p' CMakeFiles/nchat.dir/flags.make)
# These commands contain only this local build's generated compiler arguments.
# The production controller object is replaced with deterministic dialog input.
sh -c '"$1" -std=gnu++14 -fno-access-control -DNCURSES_WIDECHAR '"$defines $includes"' \
  -I"$2/src" -c "$2/utils/test-ui-feature-integration.cpp" -o test-ui-feature-integration.o' sh "$compiler" "$source_dir"
link=$(sed \
  -e 's@CMakeFiles/nchat.dir/src/main.cpp.o@test-ui-feature-integration.o@' \
  -e 's@CMakeFiles/nchat.dir/src/uicontroller.cpp.o@@' \
  -e 's@-o bin/nchat@-o test-ui-feature-integration@' \
  CMakeFiles/nchat.dir/link.txt)
sh -c "$link"
./test-ui-feature-integration
