"""Set NSDockTile inside Kitty; LaunchServices metadata alone is not a badge."""
import ctypes
import json
import os
from pathlib import Path
import re
import time
from kittens.tui.handler import result_handler


def main(args):
    pass


@result_handler(no_ui=True)
def handle_result(args, answer, target_window_id, boss):
    if args[1:] == ["enable"]:
        # Manual local shortcut only; the authorization hook denies this action
        # remotely. New Kitty instances read their configuration at startup.
        boss.allow_remote_control = "password"
        return
    if (len(args) != 3 or args[1] not in ("0", "1") or
            re.fullmatch(r"[0-9]{1,20}:[0-9]{1,20}", args[2]) is None):
        return
    objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
    objc.objc_getClass.argtypes = [ctypes.c_char_p]
    objc.objc_getClass.restype = ctypes.c_void_p
    objc.sel_registerName.argtypes = [ctypes.c_char_p]
    objc.sel_registerName.restype = ctypes.c_void_p
    send = ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value
    obj = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
    with_str = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p)(send)
    set_obj = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
    void_msg = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)(send)
    text = ctypes.CFUNCTYPE(ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)(send)
    sel = objc.sel_registerName
    app = obj(objc.objc_getClass(b"NSApplication"), sel(b"sharedApplication"))
    tile = obj(app, sel(b"dockTile"))
    label = with_str(objc.objc_getClass(b"NSString"), sel(b"stringWithUTF8String:"), b"!" if args[1] == "1" else b"")
    set_obj(tile, sel(b"setBadgeLabel:"), label)
    void_msg(tile, sel(b"display"))
    actual = obj(tile, sel(b"badgeLabel"))
    actual = (text(actual, sel(b"UTF8String")) or b"").decode() if actual else ""
    # Acknowledgement comes from Kitty's actual NSDockTile, never terminal input.
    status = {"active": args[1] == "1", "native_label": actual,
              "kitty_pid": os.getpid(), "time": time.time(), "request": args[2]}
    config_dir = Path(__file__).resolve().parent
    for name, content in (("nchat-badge-status.json", json.dumps(status)),
                          ("nchat-badge-ack", f"{args[2]} {args[1]} {actual or '-'}\n")):
        path = config_dir / name
        fd = os.open(str(path) + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
        os.replace(str(path) + ".tmp", path)
