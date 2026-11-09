#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/nchat-group-names.XXXXXX)
trap 'rm -f "$test_dir/logs.c" "$test_dir/logs.o"; rmdir "$test_dir"' EXIT
# The event router logs through nchat's C++ host. A standalone Go test needs
# these host logging functions, while all data notifications use fixture sinks.
cat > "$test_dir/logs.c" <<'EOF'
#include <stdlib.h>
#define LOG_STUB(name) \
  void name(char *file, int line, char *message) { \
    (void)line; free(file); free(message); \
  }
LOG_STUB(WmLogTrace)
LOG_STUB(WmLogDebug)
LOG_STUB(WmLogInfo)
LOG_STUB(WmLogWarning)
LOG_STUB(WmLogError)
EOF
cc -c "$test_dir/logs.c" -o "$test_dir/logs.o"
cd lib/wmchat/go
NCHAT_TEST_NATIVE_LOGS=1 go test -race -count=1 -ldflags "-extldflags=$test_dir/logs.o" \
  -run 'TestLiveGroupNamesOnJoinAndRename|TestCachedNamesPublishedBeforeNetwork|TestPreferKnownName|TestMessageEdits' .
