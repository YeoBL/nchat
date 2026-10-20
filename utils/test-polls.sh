#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
source_dir=$(pwd -P)
build_dir=${1:-build}
cmake --build "$build_dir" --target nchat
cd "$build_dir"
compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' CMakeCache.txt)
defines=$(sed -n 's/^CXX_DEFINES = //p' CMakeFiles/nchat.dir/flags.make)
includes=$(sed -n 's/^CXX_INCLUDES = //p' CMakeFiles/nchat.dir/flags.make)
tg_includes=$(sed -n 's/^CXX_INCLUDES = //p' lib/tgchat/CMakeFiles/tgchat.dir/flags.make)
sh -c '"$1" -std=gnu++17 -fno-access-control -DNCURSES_WIDECHAR '"$defines $includes $tg_includes"' \
  -I"$2/src" -I"$2/lib/tgchat/src" \
  -c "$2/utils/test-poll-dialog.cpp" -o test-poll-dialog.o' sh "$compiler" "$source_dir"
link=$(sed -e 's@CMakeFiles/nchat.dir/src/main.cpp.o@test-poll-dialog.o@' \
  -e 's@CMakeFiles/nchat.dir/src/uicontroller.cpp.o@@' \
  -e 's@-o bin/nchat@-o test-poll-dialog@' CMakeFiles/nchat.dir/link.txt)
crypto=$(sed -n 's/^OPENSSL_CRYPTO_LIBRARY:FILEPATH=//p' CMakeCache.txt)
sh -c "$link lib/libtdapi.a lib/libtdutils.a \"$crypto\""
./test-poll-dialog
