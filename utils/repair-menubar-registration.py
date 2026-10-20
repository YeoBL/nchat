#!/usr/bin/env python3
"""Repair only nchat Unread's wrongly attributed macOS menu-bar record."""
import copy
import os
from pathlib import Path
import plistlib
import tempfile

TARGET = {"bundle": {"_0": "local.nchat.unread"}}
PATH = Path.home() / "Library/Group Containers/group.com.apple.controlcenter/Library/Preferences/group.com.apple.controlcenter.plist"


def repair(records):
    result = copy.deepcopy(records)
    assert isinstance(result, list) and len(result) % 2 == 0
    found = False
    for i in range(0, len(result), 2):
        key, record = result[i:i + 2]
        assert isinstance(record, dict) and record.get("location") == key
        if key == TARGET:
            record["isAllowed"] = True
            if TARGET not in record["menuItemLocations"]:
                record["menuItemLocations"].append(copy.deepcopy(TARGET))
            found = True
        else:
            record["menuItemLocations"] = [loc for loc in record["menuItemLocations"] if loc != TARGET]
    if not found:
        result.extend([copy.deepcopy(TARGET), {
            "isAllowed": True, "location": copy.deepcopy(TARGET), "menuItemLocations": [copy.deepcopy(TARGET)]}])
    return result


def self_test():
    foreign = {"bundle": {"_0": "other.app"}}
    before = [foreign, {"isAllowed": False, "location": foreign, "menuItemLocations": [foreign, TARGET]}]
    original = copy.deepcopy(before)
    after = repair(before)
    assert before == original
    assert after[1] == {"isAllowed": False, "location": foreign, "menuItemLocations": [foreign]}
    assert after[2] == TARGET and after[3]["isAllowed"]
    assert repair(after) == after
    print("PASS: scoped repair preserves foreign app settings and is idempotent")


if __name__ == "__main__":
    import sys
    self_test()
    if "--apply" in sys.argv:
        raw = PATH.read_bytes()
        document = plistlib.loads(raw)
        records = plistlib.loads(document["trackedApplications"])
        updated = repair(records)
        if updated == records:
            print("Already correctly registered")
            sys.exit(0)
        backup = Path.home() / ".config/nchat/controlcenter-before-nchat-registration.plist"
        # Preserve the first pre-repair backup rather than overwriting it on retries.
        if not backup.exists():
            with backup.open("xb") as out:
                os.chmod(backup, 0o600)
                out.write(raw)
        document["trackedApplications"] = plistlib.dumps(updated, fmt=plistlib.FMT_BINARY)
        fd, temporary = tempfile.mkstemp(prefix=".nchat-registration-", dir=PATH.parent)
        try:
            with os.fdopen(fd, "wb") as out:
                out.write(plistlib.dumps(document, fmt=plistlib.FMT_BINARY))
                out.flush()
                os.fsync(out.fileno())
            # Do not overwrite a concurrent preference change.
            if PATH.read_bytes() != raw:
                raise RuntimeError("Menu-bar preferences changed; retry the repair")
            os.replace(temporary, PATH)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        print("Reassigned nchat Unread only; all other allowances preserved")
        print("Backup:", backup)
