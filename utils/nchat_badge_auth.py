"""Permit only nchat's fixed Dock badge operation, never general remote control."""
from pathlib import Path
import re


BADGE_HELPER = str(Path(__file__).resolve().with_name("nchat_badge.py"))


def is_cmd_allowed(pcmd, window, from_socket, extra_data):
    if not isinstance(pcmd, dict):
        return False
    payload = pcmd.get("payload", {})
    if not isinstance(payload, dict):
        return False
    args = payload.get("args", [])
    return (
        not from_socket
        and pcmd.get("cmd") == "kitten"
        and pcmd.get("no_response") is True
        and set(payload) == {"kitten", "args"}
        and payload.get("kitten") == BADGE_HELPER
        and isinstance(args, list) and len(args) == 2
        and args[0] in ("0", "1")
        and isinstance(args[1], str)
        and re.fullmatch(r"[0-9]{1,20}:[0-9]{1,20}", args[1]) is not None
    )
