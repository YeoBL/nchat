"""Current-status UI checks using temporary synthetic contacts and real PTYs."""
import argparse
import contextlib
import curses
import fcntl
import importlib.machinery
import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import pty
import select
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import time
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).with_name('nchat-presence')
loader = importlib.machinery.SourceFileLoader('presence_current', str(SCRIPT))
spec = importlib.util.spec_from_loader(loader.name, loader)
app = importlib.util.module_from_spec(spec)
loader.exec_module(app)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class Window:
    def __init__(self, keys, width, height=24):
        self.keys = iter(keys)
        self.width = width
        self.height = height
        self.frames = []
        self.lines = {}

    def keypad(self, _): pass
    def timeout(self, _): pass
    def getmaxyx(self): return self.height, self.width
    def erase(self): self.lines = {}
    def addstr(self, y, x, text, attribute): self.lines[y] = (text, attribute)
    def refresh(self): self.frames.append(dict(self.lines))
    def get_wch(self): return next(self.keys)


def terminal_check(args, width, interrupt=False):
    child, master = pty.fork()
    if child == 0:
        os.environ['TERM'] = 'xterm-256color'
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 24, width, 0, 0))
        command = [sys.executable, str(SCRIPT)]
        if width >= 94:
            command.append('current')
        os.execv(sys.executable, command + ['--dir', args.dir, '--appdir', args.appdir])
    output = bytearray()
    finished = False
    try:
        def read_for(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
        deadline = time.monotonic() + 2
        while b'Latest status' not in output and time.monotonic() < deadline:
            read_for(.05)
        check(b'Latest status' in output, 'Normal CLI did not open the status screen')
        for keys in [b'j', b'\t', b'/jamie\n', b'\x1b', b'r', b'\x1b[6~']:
            os.write(master, keys)
            read_for(.08)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 12, 40, 0, 0))
        os.kill(child, signal.SIGWINCH)
        read_for(.1)
        os.write(master, b'\x03' if interrupt else b'q')
        for _ in range(60):
            read_for(.03)
            pid, status = os.waitpid(child, os.WNOHANG)
            if pid:
                finished = True
                check(os.waitstatus_to_exitcode(status) == 0, 'Native terminal UI exited with an error')
                break
        check(finished, 'Native terminal UI did not respond to q')
        check(b'Traceback' not in output, 'Native terminal UI raised an exception')
    finally:
        if not finished:
            os.kill(child, signal.SIGKILL)
            os.waitpid(child, 0)
        os.close(master)


with tempfile.TemporaryDirectory(prefix='nchat-current-ui-') as temporary:
    root = Path(temporary)
    directory = root / 'presence'
    directory.mkdir()
    now = int(time.time() * 1000)
    profiles = {
        'Telegram_fixture': [('100', 'Jamie'), ('101', 'Alex'), ('102', '李伟 🇸🇬'), ('103', 'Hidden')],
        'WhatsAppMd_fixture': [('123@s.whatsapp.net', 'Alex'), ('456@lid', 'Fresh'), ('789@lid', 'Recent')],
    }
    for profile, contacts in profiles.items():
        folder = root / 'history' / profile
        folder.mkdir(parents=True)
        with sqlite3.connect(folder / 'db.sqlite') as connection:
            connection.execute('CREATE TABLE contacts2 (id TEXT, name TEXT, phone TEXT)')
            connection.executemany('INSERT INTO contacts2 VALUES (?, ?, ?)', [(uid, name, '') for uid, name in contacts])
        records = []
        for index, (uid, name) in enumerate(contacts):
            record = dict(ts=now - 30_000, u=uid, on=0, seen=now - 3_600_000, recent=0)
            if name == 'Fresh': record.update(on=1, seen=-1)
            if name == 'Recent': record.update(seen=-1, recent=1)
            if name == 'Jamie': record.update(ts=now - 900_000, on=1, seen=-1)
            records.append(record)
            if name == 'Jamie': records.append(dict(record, ts=now, hb=1))
            if name == 'Hidden': records.append(dict(ts=now - 10_000, u=uid, mark='hidden'))
        (directory / ('history.' + profile.encode().hex() + '.jsonl')).write_text(
            ''.join(json.dumps(record) + '\n' for record in records))
    args = argparse.Namespace(dir=str(directory), appdir=str(root), profile=None,
                              no_names=False, pattern='', plain=True)
    contacts = app.current_contacts(args)
    by_name = {item['name']: item for item in contacts}
    check(len(contacts) == 7, 'Accounts or duplicate names were merged')
    check(app.current_fields(by_name['Jamie'], now)[0] == 'Unconfirmed', 'Heartbeat renewed stale online status')
    check(by_name['Jamie']['record']['ts'] == now - 900_000, 'Report age uses a heartbeat timestamp')
    check(app.current_fields(by_name['Fresh'], now)[0] == 'Online', 'Fresh online report missing')
    boundary = dict(by_name['Fresh'], record=dict(by_name['Fresh']['record'], ts=now - 300_000))
    check(app.current_fields(boundary, now)[0] == 'Online', 'Five-minute boundary changed')
    boundary['record']['ts'] -= 1
    check(app.current_fields(boundary, now)[0] == 'Unconfirmed', 'Expired online report remains fresh')
    check(app.current_fields(by_name['Hidden'], now)[:2] == ('Unavailable', 'Unavailable'), 'Hidden record exposed old state')
    check(app.current_fields(by_name['Recent'], now)[0] == 'Seen recently', 'Coarse status became offline')
    check(app.report_age(now + 120_000, now) == 'Clock mismatch', 'Future timestamps treated as fresh')
    future = dict(by_name['Jamie'], record=dict(by_name['Jamie']['record'], ts=now + 120_000, on=0))
    check(app.current_fields(future, now)[0] == 'Unconfirmed', 'Future offline sample claims a current state')
    check(app.report_age(now - 90_000, now) == '1m ago', 'Report age is unreadable')
    check(app.readable_seen(2, now) == 'Within last week', 'Coarse last-seen timestamp corrupted')
    check(app.readable_seen(1, now) == 'Within last month', 'Monthly last-seen sentinel corrupted')
    check('Yesterday' in app.readable_seen(now - 86_400_000, now), 'Yesterday formatting missing')
    check(len(app.current_rows(contacts, 'alex', 'All')) == 2, 'Name search loses duplicate contacts')
    check(len(app.current_rows(contacts, 'alex', 'Telegram')) == 1, 'Service filter leaks rows')
    for width in (1, 12, 26, 40, 80, 120):
        for text, _, _ in app.current_lines(contacts, width, now):
            clipped = app.fit_screen(text, width)
            check(sum(max(0, app._libc.wcwidth(char)) for char in clipped) <= width, 'Row exceeds terminal width')
    check('\x1b' not in app.screen_text('Bad\x1b[31m'), 'Contact name can inject terminal controls')
    check('\u202e' not in app.screen_text('\u202eName'), 'Bidi control can reorder the interface')
    check(app.fit_screen('李伟 and a long name', 6).endswith('…'), 'Wide names are not clipped safely')
    check(app.screen_text('👍🏻 🇸🇬') == '👍 SG', 'Emoji can desynchronise terminal cell widths')
    with contextlib.redirect_stdout(io.StringIO()) as output:
        app.print_current(args)
    text = output.getvalue()
    check('WHATSAPP' in text and 'TELEGRAM' in text and 'Jamie' in text, 'Grouped plain view missing names')
    check('samples' not in text and 'heartbeats' not in text and '@s.whatsapp.net' not in text, 'Current rows contain historical clutter')
    for width in (40, 80, 120):
        keys = ['j', '\t', '/', 'j', 'a', 'm', 'i', 'e', '\n', '\x1b', 'G', 'g', 'r', curses.KEY_NPAGE, 'q']
        window = Window(keys, width)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, args)
        frames = ['\n'.join(value[0] for _, value in sorted(frame.items())) for frame in window.frames]
        check('WHATSAPP' in frames[2] and 'Jamie' not in frames[2], 'Tab does not filter the service')
        check(any('/jamie_' in frame for frame in frames), 'Search does not update as typed')
        check(any('Refreshed' in frame for frame in frames), 'Refresh completion missing')
        check(any('Alex' in frame and '123' in frame for frame in frames), 'Selected identity detail missing')
        for frame in window.frames:
            check('q Quit' in frame[23][0], 'Quit help is clipped or absent')
            for text, _ in frame.values():
                check(sum(max(0, app._libc.wcwidth(char)) for char in text) < width,
                      'Rendered frame crosses the terminal edge')
        check(any(attribute & curses.A_REVERSE for _, attribute in window.frames[0].values()),
              'Selection disappears without colour')
    for width, height in [(26, 10), (40, 12), (80, 16), (120, 40), (20, 6)]:
        window = Window(['G', 'r', 'q'], width, height)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, args)
        check(any('q Quit' in value[0] for value in window.frames[-1].values()),
              'Small terminal cannot discover exit')
        if height >= 12:
            check('Recent' in window.frames[1][height - 6][0], 'Last-contact navigation lost selection')
    for width in (26, 40, 63, 64, 71, 72, 93, 94, 95, 120):
        window = Window(['q'], width)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, args)
        heading = window.frames[0][3][0]
        check(('CONTACT' in heading) == (width >= 95), 'Column heading and row layout disagree at a width boundary')
    window = Window(['/', 'q', 'q', '\n', '\x1b', 'q'], 80)
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    check(any('/qq_' in frame[2][0] for frame in window.frames), 'Typing q in search exits the browser')
    for colour_setup in ('disabled', 'unsupported'):
        window = Window(['q'], 80)
        environment = {'NO_COLOR': '1'} if colour_setup == 'disabled' else {}
        failure = AssertionError('NO_COLOR ignored') if colour_setup == 'disabled' else curses.error('synthetic unsupported colour')
        with patch.dict(os.environ, environment, clear=True), patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=True), patch.object(curses, 'start_color', side_effect=failure):
            app.browse_current(window, args)
    window = Window(['j', 'r', 'q'], 80)
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    check(window.frames[1][18][0] == window.frames[2][18][0], 'Refresh moved the selected contact')
    window = Window(['r', 'q'], 80)
    loads = iter([contacts, OSError('synthetic refresh failure')])
    def fail_refresh(_):
        value = next(loads)
        if isinstance(value, Exception): raise value
        return value
    with patch.object(app, 'current_contacts', side_effect=fail_refresh), patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    check('retry' in window.frames[1][21][0], 'Refresh failure has no recovery action')
    check('Alex' in window.frames[1][18][0], 'Refresh failure destroyed the last good view')
    empty_args = argparse.Namespace(**dict(vars(args), dir=str(root / 'missing')))
    with contextlib.redirect_stdout(io.StringIO()) as output:
        app.print_current(empty_args)
    check('No status reports available yet' in output.getvalue(), 'First-use empty state is missing')
    no_match = argparse.Namespace(**dict(vars(args), pattern='absent-name'))
    with contextlib.redirect_stdout(io.StringIO()) as output:
        app.print_current(no_match)
    check('No contacts match' in output.getvalue(), 'Empty search feedback is missing')
    result = subprocess.run([sys.executable, str(SCRIPT), '--dir', args.dir, '--appdir', args.appdir],
                            capture_output=True, text=True, timeout=3)
    check(result.returncode == 0 and 'TELEGRAM' in result.stdout and '\x1b' not in result.stdout,
          'Piped default output needs a terminal or includes ANSI controls')
    # A larger cache still keeps only one state per contact while ignoring re-affirmations.
    path = directory / ('history.' + 'Telegram_fixture'.encode().hex() + '.jsonl')
    with path.open('a') as stream:
        stream.write((json.dumps(dict(ts=now, u='100', on=1, seen=-1, hb=1)) + '\n') * 10000)
    started = time.monotonic()
    check(len(app.current_contacts(args)) == 7, 'Latest-state memory grows with historical sample count')
    elapsed = time.monotonic() - started
    check(elapsed < 2, 'Small local-cache refresh exceeded 2 seconds')
    before = {str(path): hashlib.sha256(path.read_bytes()).digest() for path in root.rglob('*') if path.is_file()}
    terminal_check(args, 80)
    terminal_check(args, 120)
    terminal_check(args, 80, interrupt=True)
    after = {str(path): hashlib.sha256(path.read_bytes()).digest() for path in root.rglob('*') if path.is_file()}
    check(before == after, 'Current-status browsing modified source data')

print('PASS: current-state freshness, names, service groups, timestamps, Unicode width, search/navigation, refresh/recovery, compact layout, plain/empty states and real terminal resize/quit; synthetic data only')
print('Synthetic 10,000-record refresh: %.3fs; source files unchanged' % elapsed)
