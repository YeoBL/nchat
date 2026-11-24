"""Exercise the real thumbnail helper against Kitty's in-memory graphics parser."""
import base64
import os
from pathlib import Path
import pty
import select
import signal
import tempfile
import time

from kitty.fast_data_types import Screen
from kitty_tests import parse_bytes

HELPER = Path(__file__).with_name('nchat-thumbnails').resolve()


def run_helper(arguments, cache):
    child, master = pty.fork()
    if child == 0:
        os.environ.pop('STY', None)
        os.environ['XDG_CACHE_HOME'] = str(cache)
        os.execv(str(HELPER), [str(HELPER), *map(str, arguments)])
    output = bytearray()
    finished = False
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if select.select([master], [], [], .03)[0]:
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    pass
            pid, status = os.waitpid(child, os.WNOHANG)
            if pid:
                finished = True
                if os.waitstatus_to_exitcode(status):
                    raise AssertionError(output.decode(errors='replace'))
                return bytes(output)
        raise AssertionError('Thumbnail helper did not exit')
    finally:
        if not finished:
            os.kill(child, signal.SIGKILL)
            os.waitpid(child, 0)
        os.close(master)


with tempfile.TemporaryDirectory(prefix='nchat-thumbnail-placements-') as directory:
    root = Path(directory)
    image = root / "synthetic image's.png"
    image.write_bytes(base64.b64decode(
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a2uoAAAAASUVORK5CYII='))
    screen = Screen(None, 24, 80, 0)
    initial = run_helper(['--frame', '', image, 1, 1, 8, 8, 200, 0], root / 'cache')
    parse_bytes(screen, initial)
    uploaded = screen.grman.image_for_client_id(200)
    if not uploaded or b'a=T' not in initial:
        raise AssertionError('Initial image was not uploaded')
    moved = run_helper(['--frame', '', image, 10, 5, 8, 8, 200, 1], root / 'cache')
    if b'a=p' not in moved or b'a=T' in moved or b'd=A' in moved or b'f=100' in moved:
        raise AssertionError('Moving an image retransmits or clears its data')
    parse_bytes(screen, moved)
    retained = screen.grman.image_for_client_id(200)
    if not retained or retained['internal_id'] != uploaded['internal_id'] or retained['refs.count'] != 1 or screen.grman.image_count != 1:
        raise AssertionError('Moving the placement replaced the image resource')
    removed = run_helper(['--frame', '200'], root / 'cache')
    parse_bytes(screen, removed)
    if screen.grman.image_for_client_id(200):
        raise AssertionError('Off-screen image resource was not freed')
    legacy = run_helper([image, 1, 1, 8, 8], root / 'cache')
    parse_bytes(screen, legacy)
    if not screen.grman.image_for_client_id(200):
        raise AssertionError('Legacy five-argument thumbnail callers broke')
    print('PASS: native Kitty loads once, moves the same image resource with a placement command, frees off-screen data and supports legacy callers')
