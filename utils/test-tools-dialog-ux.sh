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
# Only input is substituted; all production dialog/model implementations link.
sh -c '"$1" -std=gnu++14 -fno-access-control -DNCURSES_WIDECHAR '"$defines $includes"' \
  -I"$2/src" -c "$2/utils/test-tools-dialog-ux.cpp" -o test-tools-dialog-ux.o' sh "$compiler" "$source_dir"
link=$(sed \
  -e 's@CMakeFiles/nchat.dir/src/main.cpp.o@test-tools-dialog-ux.o@' \
  -e 's@CMakeFiles/nchat.dir/src/uicontroller.cpp.o@@' \
  -e 's@-o bin/nchat@-o test-tools-dialog-ux@' \
  CMakeFiles/nchat.dir/link.txt)
sh -c "$link"
./test-tools-dialog-ux
