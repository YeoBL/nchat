"""Reproduce Screen's full-input-queue spin without accounts or a real UI."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

SCREEN = '/opt/homebrew/bin/screen'
CONFIG = Path(__file__).with_name('screen-scroll.conf').resolve()


def check(protected):
    with tempfile.TemporaryDirectory(prefix='nchat-screen-scroll-') as temporary:
        root = Path(temporary)
        sockets = root / 'sockets'
        sockets.mkdir(mode=0o700)
        config = root / 'screenrc'
        config.write_text('startup_message off\ndefutf8 on\n' +
                          (f'source {CONFIG}\n' if protected else ''))
        environment = dict(os.environ, TERM='xterm-kitty', SCREENDIR=str(sockets))
        program = ("import os,time,tty; from pathlib import Path; tty.setraw(0); "
                   f"Path({str(root / 'child')!r}).write_text(str(os.getpid())); "
                   "os.write(1,b'\\x1b[?1hREADY\\r\\n'); time.sleep(15)")
        # The normal key path must still deliver the complete navigation bytes.
        program = program.replace('time.sleep(15)',
            "data=bytearray();\nwhile not data.endswith(b'!'): data.extend(os.read(0,1))\n" +
            f"Path({str(root / 'keys')!r}).write_bytes(data); time.sleep(15)")
        client, master = pty.fork()
        if client == 0:
            os.environ.update(environment)
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 96, 0, 0))
            os.execv(SCREEN, [SCREEN, '-c', str(config), '-S', 'scroll-test',
                             '/usr/bin/python3', '-c', program])
        try:
            startup = bytearray()
            deadline = time.monotonic() + 8
            while not (root / 'child').exists() and time.monotonic() < deadline:
                if select.select([master], [], [], .05)[0]:
                    startup.extend(os.read(master, 65536))
            if not (root / 'child').exists():
                fcntl.fcntl(master, fcntl.F_SETFL, os.O_NONBLOCK)
                try:
                    detail = os.read(master, 65536).decode(errors='replace')
                except OSError:
                    detail = ''
                raise RuntimeError('Synthetic child did not start: ' + detail)
            # Freeze the synthetic consumer, then overflow both the PTY and
            # Screen queues with application-mode arrows, as rapid scrolling can.
            fcntl.fcntl(master, fcntl.F_SETFL, os.O_NONBLOCK)
            stop = threading.Event()
            def drain():
                while not stop.is_set():
                    try:
                        if select.select([master], [], [], .05)[0]:
                            os.read(master, 65536)
                    except OSError:
                        break
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            session = next(p.name for p in sockets.iterdir() if p.name.endswith('.scroll-test'))
            keys = b'\x1bOA\x1bOB\x1bOD\x1bOC\x1b[5~\x1b[6~!'
            os.write(master, keys)
            deadline = time.monotonic() + 2
            while not (root / 'keys').exists() and time.monotonic() < deadline:
                time.sleep(.02)
            if not (root / 'keys').exists():
                raise AssertionError('Normal navigation was not delivered')
            if protected and (root / 'keys').read_bytes() != keys:
                raise AssertionError('Navigation bytes changed')
            data = b'\x1bOA' * 12000
            deadline = time.monotonic() + 1
            while data and time.monotonic() < deadline:
                try:
                    data = data[os.write(master, data):]
                except BlockingIOError:
                    time.sleep(.005)
            probe = root / 'synthetic-screen'
            result = subprocess.run([SCREEN, '-S', session, '-p', '0', '-X', 'hardcopy', str(probe)],
                                    env=environment, capture_output=True, timeout=2)
            deadline = time.monotonic() + 1
            while not probe.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            responsive = result.returncode == 0 and probe.exists()
            detail = repr((result.stdout, result.stderr))
            if responsive != protected:
                raise AssertionError(f'protected={protected}, responsive={responsive}, {detail}, startup={startup!r}')
            print('PASS:', 'navigation remains responsive with a full queue' if protected
                  else 'original Screen default mapping reproduces the freeze')
        finally:
            if 'stop' in locals():
                stop.set()
                reader.join(timeout=1)
            victims = [client]
            if (root / 'child').exists():
                victims.append(int((root / 'child').read_text()))
            victims.extend(int(p.name.split('.')[0]) for p in sockets.iterdir()
                           if p.name.split('.')[0].isdigit())
            for pid in set(victims):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            os.close(master)
            os.waitpid(client, 0)


check(False)
check(True)
