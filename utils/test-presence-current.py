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


def terminal_check(args, width, interrupt=False, linking=False):
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
        if linking:
            preference = Path(args.appdir) / 'presence-contact-links.json'
            os.write(master, b'm')
            read_for(.1)
            os.write(master, b'\x1b')
            read_for(.15)
            check(not preference.exists(), 'Native link cancellation saved preferences')
            os.write(master, b'm\n')
            deadline = time.monotonic() + 2
            while not preference.exists() and time.monotonic() < deadline:
                read_for(.03)
            check(preference.exists() and json.loads(preference.read_text())['links'],
                  'Native Link did not reach the preference writer')
            os.write(master, b'u')
            deadline = time.monotonic() + 2
            while json.loads(preference.read_text())['links'] and time.monotonic() < deadline:
                read_for(.03)
            check(not json.loads(preference.read_text())['links'], 'Native Unlink failed')
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
    people = app.current_people(args)
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
    check(len(app.current_rows(people, 'alex', 'All')) == 2, 'Same-name people were silently joined')
    check(len(app.current_rows(people, 'alex', 'Telegram')) == 1, 'Service filter leaks rows')
    for width in (1, 12, 26, 40, 80, 120):
        for text, _, _ in app.current_lines(people, width, now):
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
        check(any('jamie_' in frame for frame in frames), 'Search does not update as typed')
        check(any('Refreshed' in frame for frame in frames), 'Refresh completion missing')
        check(any('Alex' in frame and '123' in frame for frame in frames), 'Selected identity detail missing')
        for frame in window.frames:
            check('q Quit' in frame[23][0], 'Quit help is clipped or absent')
            for text, _ in frame.values():
                check(sum(max(0, app._libc.wcwidth(char)) for char in text) < width,
                      'Rendered frame crosses the terminal edge')
        check(any(attribute & curses.A_REVERSE for _, attribute in window.frames[0].values()),
              'Selection disappears without colour')
    for width, height in [(26, 10), (40, 12), (40, 13), (80, 16), (120, 40), (20, 6)]:
        window = Window(['G', 'r', 'q'], width, height)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, args)
        check(any('q Quit' in value[0] for value in window.frames[-1].values()),
              'Small terminal cannot discover exit')
        if height >= (13 if width < 45 else 12):
            check(people[-1]['name'].split()[0] in window.frames[1][height - 6][0], 'Last-contact navigation lost selection')
    for width in (26, 40, 63, 64, 71, 72, 93, 94, 95, 120):
        window = Window(['q'], width)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, args)
        heading = window.frames[0][3][0]
        check(('WHATSAPP' in heading) == (width >= 45), 'Column heading and row layout disagree at a width boundary')
    window = Window(['/', 'q', 'q', '\n', '\x1b', 'q'], 80)
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    check(any('qq_' in frame[2][0] for frame in window.frames), 'Typing q in search exits the browser')
    alex_tg = next(item for item in contacts if item['key'] == ('Telegram_fixture', '101'))
    alex_wa = next(item for item in contacts if item['key'] == ('WhatsAppMd_fixture', '123@s.whatsapp.net'))
    alex_person = {'name': 'Alex',
                   'key': (alex_tg['key'], alex_wa['key']),
                   'members': [alex_tg, alex_wa], 'manual': False}
    # Online intervals: exact end, unknown end, hidden end, open run, rollback.
    t0 = now - 3_600_000
    sequence = [
        dict(ts=t0, u='x', on=1, seen=-1, recent=0),
        dict(ts=t0 + 900_000, u='x', on=0, seen=t0 + 840_000, recent=0),
        dict(ts=t0 + 1_800_000, u='x', on=1, seen=-1, recent=0),
        dict(ts=t0 + 1_900_000, u='x', on=0, seen=-1, recent=0),
        dict(ts=t0 + 2_700_000, u='x', on=1, seen=-1, recent=0),
        dict(ts=t0 + 2_800_000, u='x', mark='hidden'),
        dict(ts=t0 + 3_600_000, u='x', on=1, seen=-1, recent=0),
        dict(ts=t0 + 3_600_000, u='x', on=1, seen=-1, recent=0, hb=1),
    ]
    intervals = app.online_intervals(sequence)
    check(len(intervals) == 4, 'Interval derivation misses or invents sessions')
    check(intervals[0]['end_kind'] == 'exact' and intervals[0]['end_ts'] == t0 + 840_000,
          'Exact seen timestamp not used as the interval end')
    check(intervals[1]['end_kind'] == 'unknown' and intervals[1]['closed_at'] == t0 + 1_900_000,
          'Offline without seen does not close with unknown end')
    check(intervals[2]['end_kind'] == 'hidden', 'Hidden marker does not close the run')
    check(intervals[3]['end_kind'] == 'open', 'Trailing online run is not marked open')
    check(app.duration_text(t0, t0 + 900_000) == '~15m', 'Duration is not an estimate of the interval')
    check(app.duration_text(t0, None) == '?', 'Unknown end must not invent a duration')
    rolled = app.online_intervals([
        dict(ts=t0 + 4_000_000, u='x', on=1, seen=-1, recent=0),
        dict(ts=t0 - 10, u='x', on=1, seen=-1, recent=0),
    ])
    check(len(rolled) == 2 and rolled[0]['end_kind'] == 'unknown',
          'Backward timestamp jump must break the run')
    # Merged view: newest first across services; header carries both states.
    alex_intervals = app.person_intervals(alex_person, args)
    check(alex_intervals == sorted(alex_intervals, key=lambda item: item['start_ts'], reverse=True),
          'Merged intervals are not newest first')
    check('Alex' in app.history_header(alex_person, now, 80)
          and ('TG' in app.history_header(alex_person, now, 80)
               or 'WA' in app.history_header(alex_person, now, 80)),
          'History header misses the name or a service state')
    for width in (40, 60, 80, 120):
        line = app.interval_line(dict(start_ts=t0, end_ts=t0 + 900_000,
                                      end_kind='exact', service='WhatsApp'), width, now)
        check(sum(max(0, app._libc.wcwidth(char)) for char in line) <= width,
              'Interval row exceeds terminal width')
        check('~15m' in line and ('→' in line or '│' in line), 'Interval row lost its range or duration')
    window = Window(['j', 'G', 'r', 'q'], 80, 24)
    synthetic = [
        dict(start_ts=t0, end_ts=t0 + 900_000, end_kind='exact', service='WhatsApp'),
        dict(start_ts=t0 - 7_200_000, end_ts=None, end_kind='open', service='Telegram'),
    ]
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False), \
         patch.object(app, 'person_intervals', return_value=synthetic):
        app.browse_person(window, args, alex_person)
    frames = ['\n'.join(value[0] for _, value in sorted(frame.items())) for frame in window.frames]
    check('Online periods' in frames[0], 'Person table missing its heading')
    check(any('START' in frame and 'LENGTH' in frame for frame in frames), 'Wide table lacks column headings')
    check(any('~15m' in frame and 'open?' in frame for frame in frames), 'Interval rows missing')
    check('~ = estimate' in frames[-1] and 'q/Esc Back' in frames[-1], 'Table footer hints missing')
    window = Window(['\n', 'q', 'q'], 80, 24)
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    frames = ['\n'.join(value[0] for _, value in sorted(frame.items())) for frame in window.frames]
    check(any('Online periods' in frame for frame in frames), 'Enter does not open the online table')
    check('Latest status' in frames[-1], 'Back from the table does not restore the list')
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
    check(len(app.current_people(args)) == 7, 'Latest-state memory grows with historical sample count')
    elapsed = time.monotonic() - started
    check(elapsed < 2, 'Small local-cache refresh exceeded 2 seconds')
    before = {str(path): hashlib.sha256(path.read_bytes()).digest() for path in root.rglob('*') if path.is_file()}
    terminal_check(args, 80)
    terminal_check(args, 120)
    terminal_check(args, 80, interrupt=True)
    after = {str(path): hashlib.sha256(path.read_bytes()).digest() for path in root.rglob('*') if path.is_file()}
    check(before == after, 'Current-status browsing modified source data')

    # Cross-service people retain each platform's latest state independently.
    telegram = dict(by_name['Jamie'], phone='447700900123')
    whatsapp = dict(by_name['Fresh'], phone='447700900123')
    with patch.object(app, 'current_contacts', return_value=[telegram, whatsapp]):
        combined = app.current_people(args)
    check(len(combined) == 1 and len(combined[0]['members']) == 2, 'A matching phone did not produce one person')
    check(app.service_contact(combined[0], 'Telegram')['record']['on'] == 1,
          'Telegram observation was overwritten by WhatsApp')
    check('Unconfirmed' in app.service_summary(combined[0], 'Telegram', now, 40)
          and 'Online' in app.service_summary(combined[0], 'WhatsApp', now, 40),
          'Service columns conflate stale and fresh online reports')
    for width in (44, 64, 80, 120):
        paired = app.current_lines(combined, width, now)
        check(len(paired) == 1 and paired[0][0].count('│') == 2, 'Wide view is not one row with two service columns')
    check('—' in app.service_summary(people[0], 'WhatsApp', now, 30), 'Missing service is shown as offline')
    for value in ['+44••••123', '+44∙∙123', '12345678@lid', '1234', '++447700900123']:
        check(not app.contact_phone(value), 'Masked or opaque identifier became a matching phone')
    check(app.contact_phone('+44 (7700) 900-123') == '447700900123', 'Phone punctuation prevents matching')
    check(app.contact_phone('00447700900123') == '447700900123', 'International prefix prevents matching')
    same_names = [dict(telegram, phone='', name='Alex Smith'), dict(whatsapp, phone='', name='Alex Smith')]
    link_args = argparse.Namespace(**dict(vars(args), appdir=str(root / 'link-preferences')))
    with patch.object(app, 'current_contacts', return_value=same_names):
        check(len(app.current_people(link_args)) == 2, 'An identical name silently merged two identities')
        window = Window(['m', '\x1b', 'q'], 80)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, link_args)
        link_path = Path(link_args.appdir) / 'presence-contact-links.json'
        check(not link_path.exists(), 'Cancelling the link picker saved a link')
        window = Window(['m', '\n', 'q'], 80)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, link_args)
        linked = app.current_people(link_args)
        check(len(linked) == 1 and linked[0]['manual'], 'Confirmed picker selection was not saved')
        check(link_path.stat().st_mode & 0o777 == 0o600, 'Manual identity preferences are not private')
        check(any('Contacts linked locally' in frame.get(21, ('', 0))[0] for frame in window.frames), 'Link completion feedback missing')
        with contextlib.redirect_stdout(io.StringIO()) as output:
            app.print_current(link_args)
        check(output.getvalue().count('Alex Smith') == 1, 'Plain output repeats a confirmed person')
        previous = link_path.read_bytes()
        try:
            app.save_contact_link(link_args, pair=(('Telegram_fixture', '\x00bad'), whatsapp['key']))
            check(False, 'Malformed identity was saved')
        except ValueError:
            pass
        check(link_path.read_bytes() == previous, 'Invalid identity changed saved links')
        with patch.object(app.os, 'replace', side_effect=OSError('synthetic write failure')):
            try:
                app.save_contact_link(link_args, remove=linked[0]['key'])
                check(False, 'Failed preference write returned success')
            except OSError:
                pass
        check(link_path.read_bytes() == previous, 'Failed link save lost existing preferences')
        check(not list(Path(link_args.appdir).glob('.presence-contact-links-*')), 'Failed save left temporary copies')
        window = Window(['u', 'q'], 80)
        with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
            app.browse_current(window, link_args)
        check(len(app.current_people(link_args)) == 2 and app.read_contact_links(link_args) == [],
              'Unlink did not restore separate rows')
    pair_one = (telegram['key'], whatsapp['key'])
    pair_two = (('Telegram_fixture', 'another'), ('WhatsAppMd_fixture', 'second@lid'))
    code = ('import runpy,sys,types,json; '
            'm=runpy.run_path(sys.argv[1]); '
            'm["save_contact_link"](types.SimpleNamespace(appdir=sys.argv[2]), '
            'pair=tuple(tuple(x) for x in json.loads(sys.argv[3])))')
    writers = [subprocess.Popen([sys.executable, '-c', code, str(SCRIPT), link_args.appdir, json.dumps(pair)])
               for pair in (pair_one, pair_two)]
    check(all(writer.wait(timeout=5) == 0 for writer in writers), 'Concurrent preference writer failed')
    check(len(app.read_contact_links(link_args)) == 2, 'Concurrent writers lost a link')
    broken = argparse.Namespace(**dict(vars(link_args), appdir=str(root / 'bad-links')))
    Path(broken.appdir).mkdir(mode=0o700)
    damaged = Path(broken.appdir) / 'presence-contact-links.json'
    damaged.write_text('invalid-json')
    damaged.chmod(0o600)
    with patch.object(app, 'current_contacts', return_value=[telegram, whatsapp]), contextlib.redirect_stderr(io.StringIO()):
        check(len(app.current_people(broken)) == 1, 'Broken manual links prevent automatic matching')
    try:
        app.save_contact_link(broken, pair=pair_one)
        check(False, 'Malformed preferences were silently overwritten')
    except ValueError:
        pass
    check(damaged.read_text() == 'invalid-json', 'Malformed preference recovery destroyed the file')
    phone_root = root / 'phone-cache'
    phone_presence = phone_root / 'presence'
    phone_presence.mkdir(parents=True)
    fixtures = [
        ('Telegram_phone', '313030', 'Alex Doe', '+44 (7700) 900-123', 0),
        ('WhatsAppMd_phone', '987654321@lid', 'A.D.', '447700900123', 0),
        ('WhatsAppMd_second', '447700900123@s.whatsapp.net', '', '', 1),
    ]
    for profile, uid, name, phone, online in fixtures:
        folder = phone_root / 'history' / profile
        folder.mkdir(parents=True)
        with sqlite3.connect(folder / 'db.sqlite') as connection:
            connection.execute('CREATE TABLE contacts2 (id TEXT, name TEXT, phone TEXT)')
            connection.execute('INSERT INTO contacts2 VALUES (?, ?, ?)', (uid, name, phone))
        record = dict(ts=now - (1000 if online else 30_000), u=uid, on=online, seen=now - 60_000)
        (phone_presence / ('history.' + profile.encode().hex() + '.jsonl')).write_text(json.dumps(record) + '\n')
    phone_args = argparse.Namespace(**dict(vars(args), appdir=str(phone_root), dir=str(phone_presence)))
    phone_people = app.current_people(phone_args)
    check(len(phone_people) == 1 and len(phone_people[0]['members']) == 3,
          'Real cache phone lookup did not join Telegram, WhatsApp LID and phone identities')
    check(phone_people[0]['name'] == 'Alex Doe', 'A numeric fallback replaced a known full name')
    check(app.service_contact(phone_people[0], 'WhatsApp')['record']['on'] == 1,
          'Multiple account records discarded the newest service report')
    check(len(app.current_rows(phone_people, 'a.d.', 'All')) == 1, 'Search lost an alias on the second service')
    check(len(app.current_rows(phone_people, '+44 (7700) 900-123', 'All')) == 1,
          'Formatted phone search does not match cached phone metadata')
    window = Window(['\x06', 'j', 'a', 'm', 'i', 'e', curses.KEY_DOWN, 'q'], 80)
    with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
        app.browse_current(window, args)
    check(any('SEARCH name/number' in frame[2][0] for frame in window.frames), 'Ctrl+F does not focus labelled search')
    check(any('jamie_' in frame[2][0] for frame in window.frames), 'Ctrl+F search does not update')
    # A saved counterpart can exist in the address book without a presence sample.
    cached = phone_root / 'history' / 'WhatsAppMd_phone' / 'db.sqlite'
    with sqlite3.connect(cached) as connection:
        connection.execute('ALTER TABLE contacts2 ADD COLUMN isSelf INTEGER DEFAULT 0')
        connection.executemany('INSERT INTO contacts2 VALUES (?, ?, ?, ?)', [
            ('555555555@lid', 'Quinn Search', '+44 7700 900124', 0),
            ('777777777@lid', 'Myself', '+44 7700 900125', 1),
            ('123456789@g.us', 'Group title', '', 0),
        ])
    all_cached = app.current_people(phone_args, include_cached=True)
    check(any(person['name'] == 'Quinn Search' for person in all_cached), 'Picker omits an unsampled cached contact')
    check(not any(person['name'] in ('Myself', 'Group title') for person in all_cached), 'Picker includes self or group contacts')
    source = dict(phone_people[0], members=[app.service_contact(phone_people[0], 'Telegram')])
    for query in ('quinn', '+44 (7700) 900124'):
        window = Window(list(query) + ['\n'], 80)
        chosen = app.pick_person_link(window, source, all_cached)
        check(chosen and chosen['name'] == 'Quinn Search', 'Typing a name/number needs extra focus or a second Enter')
    window = Window(['q', '\x1b'], 80)
    check(app.pick_person_link(window, source, all_cached) is None, 'Escape cannot cancel typed picker search')
    check(any('q_' in frame[2][0] for frame in window.frames), 'Typing q in the picker cancels instead of searching')
    target = next(person for person in all_cached if person['name'] == 'Quinn Search')
    app.save_contact_link(phone_args, pair=(by_name['Jamie']['key'], target['members'][0]['key']))
    with patch.object(app, 'current_contacts', return_value=[by_name['Jamie']]):
        hydrated = app.current_people(phone_args)
    counterpart = app.service_contact(hydrated[0], 'WhatsApp')
    check(counterpart and counterpart['record'] is None and app.current_fields(counterpart, now)[0] == 'No report',
          'Linked unsampled contact is missing or claims an observation')
    with contextlib.redirect_stdout(io.StringIO()) as output:
        app.print_current(phone_args)
    check(output.getvalue().count('Alex Doe') == 1, 'Normal plain entry repeats a matched person')
    with patch.object(app, 'current_contacts', return_value=[
        dict(contacts[0], key=('Telegram_pages', str(index)), identifier=str(index),
             name='Person%02d' % index, phone='') for index in range(30)]):
        for width, expected in [(80, 'Person14'), (40, 'Person04')]:
            window = Window([curses.KEY_NPAGE, 'q'], width)
            with patch.object(curses, 'curs_set'), patch.object(curses, 'has_colors', return_value=False):
                app.browse_current(window, args)
            check(expected in window.frames[1][18][0], 'Paging uses physical lines instead of people')
    terminal_check(args, 80, linking=True)

print('PASS: paired service columns, phone matching, manual link/cancel/unlink, private atomic/concurrent preferences, freshness, names, navigation, resize and legacy current-view checks; synthetic data only')
print('Synthetic 10,000-record refresh: %.3fs; source files unchanged' % elapsed)
