#!/bin/bash
# test-presence-poller.sh
#
# Builds and runs the standalone PresencePoller regression test in a private
# temp directory, then validates every generated JSONL line with python3's
# json parser. No real protocols, accounts or message data are involved.
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/nchat-presence-test.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++14 -Wall -Wextra -pedantic utils/test-presence-poller.cpp -o "$test_dir/presence-poller"
"$test_dir/presence-poller" "$test_dir"
python3 - "$test_dir" <<'EOF'
import json
import sys
import glob
root = sys.argv[1]
files = sorted(glob.glob(root + "/history.*.jsonl"))
assert files, "no history files generated"
for path in files:
    with open(path) as fh:
        for line_number, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            assert isinstance(record.get("ts"), int), (path, line_number)
            assert isinstance(record.get("u"), str), (path, line_number)
            if "mark" in record:
                assert record["mark"] == "hidden", (path, line_number)
            else:
                assert record.get("on") in (0, 1), (path, line_number)
                assert isinstance(record.get("seen"), int), (path, line_number)
                assert record.get("recent") in (0, 1), (path, line_number)
print("JSONL validation passed for %d history file(s)" % len(files))
EOF
