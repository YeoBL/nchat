"""Replay every synthetic UI frame directly and through an isolated Screen.

Run with kitty +runpy and the test directory. All terminal grids are in memory.
"""
import fcntl
import json
import runpy
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

from kitty.fast_data_types import Screen
from kitty_tests import parse_bytes

root = Path(sys.argv[1])


def snapshot(path):
    lines = path.with_suffix(".expected").read_text().splitlines()
    rows, columns = map(int, lines[0].split())
    if len(lines[1:]) != rows:
        raise AssertionError("Invalid expected row count")
    cells = path.with_suffix(".cells").read_text().splitlines()
    if len(cells) != rows or any(len(row) != columns for row in cells):
        raise AssertionError("Invalid expected cell grid")
    return rows, columns, (lines[1:], cells)


def check_grid(screen, expected, route, frame):
    lines, cells = expected
    for row, text in enumerate(lines):
        actual = str(screen.line(row)).rstrip(" ")
        if actual != text.rstrip(" "):
            raise AssertionError((route, frame, row, actual, text.rstrip(" ")))
        for column, character in enumerate(cells[row]):
            if character == "\uffff":
                continue
            actual = screen.line(row)[column]
            if actual in ("", "\x00"):
                actual = " "
            if actual != character:
                raise AssertionError((route, frame, row, column, actual, character))


# The embedded interpreter runs with -OO. Comparisons must raise explicitly.
control = Screen(None, 1, 8, 0)
parse_bytes(control, b"A")
check_grid(control, (["A"], ["A       "]), "positive control", 0)
try:
    check_grid(control, (["A"], ["B       "]), "negative control", 0)
except AssertionError:
    pass
else:
    raise RuntimeError("Terminal cell comparison did not reject a wrong position")


emoji_paint = runpy.run_path(str(Path(__file__).with_name('nchat_emoji.py')))['paint']
emoji_decode = runpy.run_path(str(Path(__file__).with_name('nchat_emoji.py')))['decode']


def unwrap_screen(data):
    result = bytearray()
    position = 0
    while position < len(data):
        if data[position:position + 2] != b'\x1bP':
            result.append(data[position])
            position += 1
            continue
        position += 2
        escaped = False
        while position < len(data):
            char = data[position]
            position += 1
            if escaped and char == ord('\\'):
                break
            if char == 27:
                if escaped:
                    result.append(27)
                escaped = True
            else:
                if escaped:
                    result.append(27)
                result.append(char)
                escaped = False
    return bytes(result)


def feed(screen, data, wrapped=False):
    if wrapped:
        data = unwrap_screen(data)
    marker = b'\x1bP@kitty-cmd'
    while marker in data:
        begin = data.index(marker)
        parse_bytes(screen, data[:begin])
        end = data.index(b'\x1b\\', begin)
        command = json.loads(data[begin + len(marker):end])
        payload = command['payload']
        if command['cmd'] != 'kitten' or Path(payload['kitten']).name != 'nchat_emoji.py':
            raise AssertionError('Unexpected remote operation in rendering fixture')
        rows = emoji_decode(payload['args'][0])
        if rows is None:
            raise AssertionError('Invalid emoji rendering request')
        before = screen.cursor.copy()
        emoji_paint(screen, rows)
        if screen.cursor != before:
            raise AssertionError(('Emoji repaint changed the input cursor', repr(before), repr(screen.cursor)))
        data = data[end + 2:]
    parse_bytes(screen, data)


def check_screen_route(binary, fixture, frames):
    rows, columns, _ = snapshot(frames[0])
    terminal = Screen(None, rows, columns, 0)
    session = f"nchat-render-test-{os.getpid()}"
    with tempfile.TemporaryDirectory(prefix="screen-render-", dir=root) as temporary:
        config = Path(temporary) / "screenrc"
        config.write_text("startup_message off\ndefutf8 on\ndefscrollback 0\n")
        child, master = pty.fork()
        if child == 0:
            os.environ["TERM"] = "xterm-kitty"
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            replay = """import os,sys,time,tty
from pathlib import Path
tty.setraw(0)
offset = 0
for name in sys.argv[1:]:
    data = Path(name).read_bytes()
    for start in range(offset, len(data), 7):
        os.write(1, data[start:start + 7])
        time.sleep(0.0001)
    offset = len(data)
    os.read(0, 1)
time.sleep(30)
"""
            os.execv(binary, [binary, "-c", str(config), "-S", session, "-U",
                             "/usr/bin/python3", "-c", replay, *map(str, frames)])
        try:
            for frame, path in enumerate(frames):
                output = bytearray()
                deadline = time.monotonic() + 12
                last_data = time.monotonic()
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.05)[0]:
                        data = os.read(master, 65536)
                        if not data:
                            break
                        output.extend(data)
                        last_data = time.monotonic()
                    elif output and time.monotonic() - last_data > 0.2:
                        break
                if not output:
                    raise AssertionError((fixture, frame, "Screen produced no output"))
                feed(terminal, bytes(output))
                check_grid(terminal, snapshot(path)[2], fixture + "/Screen", frame)
                os.write(master, b"x")
        finally:
            subprocess.run([binary, "-S", session, "-X", "quit"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
            os.close(master)
            os.waitpid(child, 0)


binary = "/opt/homebrew/bin/screen"
if not Path(binary).exists():
    binary = shutil.which("screen")
for fixture in sorted((root / "history-render").iterdir()):
    frames = sorted(fixture.glob("*.raw"), key=lambda path: int(path.stem))
    if len(frames) != 20:
        raise AssertionError("Missing UI frames")
    rows, columns, _ = snapshot(frames[0])
    terminal = Screen(None, rows, columns, 0)
    previous_length = 0
    for frame, path in enumerate(frames):
        rows, columns, expected = snapshot(path)
        data = path.read_bytes()
        feed(terminal, data[previous_length:], fixture.name.startswith('screen'))
        previous_length = len(data)
        check_grid(terminal, expected, fixture.name + "/direct", frame)
    if binary and fixture.name.startswith("screen"):
        check_screen_route(binary, fixture.name, frames)
    print(f"PASS: every {fixture.name} frame matches with independent chat-list/history redraws and one-message scrolling")
