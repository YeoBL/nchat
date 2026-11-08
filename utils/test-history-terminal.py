"""Compare synthetic ncurses output with Kitty's independent terminal parser.

Run with kitty +runpy and a test directory, as in test-notifications.sh.
This creates an in-memory Screen and accesses no running terminal or account.
"""
import sys
import fcntl
import os
import pty
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time
from pathlib import Path

from kitty.fast_data_types import Screen
from kitty_tests import parse_bytes

root = Path(sys.argv[1])
expected = (root / "history-render.expected").read_text().splitlines()
assert len(expected) == 24


def check_grid(data, route):
    screen = Screen(None, 24, 96, 0)
    parse_bytes(screen, data)
    for row, text in enumerate(expected):
        actual = str(screen.line(row)).rstrip(" ")
        assert actual == text.rstrip(" "), (route, row, actual, text.rstrip(" "))
    print(f"PASS: eight history redraws match the terminal grid {route}")


check_grid((root / "history-render.raw").read_bytes(), "directly")

# Exercise the user's multiplexer as well, with an independent synthetic session.
screen_binary = "/opt/homebrew/bin/screen"
if not Path(screen_binary).exists():
    screen_binary = shutil.which("screen")
if screen_binary:
    session = f"nchat-render-test-{os.getpid()}"
    with tempfile.TemporaryDirectory(prefix="screen-render-", dir=root) as temporary:
        config = Path(temporary) / "screenrc"
        config.write_text("startup_message off\ndefutf8 on\ndefscrollback 0\n")
        child, master = pty.fork()
        if child == 0:
            os.environ["TERM"] = "xterm-kitty"
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 96, 0, 0))
            replay = "import os,sys,time; from pathlib import Path; os.write(1,Path(sys.argv[1]).read_bytes()); time.sleep(30)"
            os.execv(screen_binary, [screen_binary, "-c", str(config), "-S", session,
                                    "-U", "/usr/bin/python3", "-c", replay,
                                    str(root / "history-render.raw")])
        output = bytearray()
        deadline = time.monotonic() + 4
        last_data = time.monotonic()
        try:
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    data = os.read(master, 65536)
                    if not data:
                        break
                    output.extend(data)
                    last_data = time.monotonic()
                elif output and time.monotonic() - last_data > 0.5:
                    break
        finally:
            subprocess.run([screen_binary, "-S", session, "-X", "quit"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
            os.close(master)
            os.waitpid(child, 0)
        check_grid(bytes(output), "through Screen")
