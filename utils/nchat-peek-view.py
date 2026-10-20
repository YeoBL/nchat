#!/usr/bin/env python3
"""Read cached conversations without opening chats or sending read receipts."""
import argparse
import contextlib
from datetime import datetime
import curses
import ctypes
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import sqlite3
import stat
import struct
import subprocess
import sys
import termios
import tempfile
import time

def config_root(value=None, environ=None):
    environ = os.environ if environ is None else environ
    root = value or environ.get('NCHAT_CONFIG_DIR')
    if not root:
        home = Path(environ.get('HOME') or Path.home())
        legacy = home / '.nchat'
        root = legacy if legacy.is_dir() else Path(environ.get('XDG_CONFIG_HOME') or (home / '.config')) / 'nchat'
    return Path(root).expanduser().resolve()


def find_helper(name, environ_name=None):
    override = os.environ.get(environ_name, '') if environ_name else ''
    if override:
        return override
    here = Path(__file__).resolve().parent
    for candidate in (here / name, here.parent / 'bin' / name):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which(name)


THUMBNAILS = find_helper('nchat-thumbnails', 'NCHAT_THUMBNAILS')
IMAGES = {'.jpg', '.jpeg', '.png', '.gif', '.webp', '.heic', '.avif', '.bmp', '.tif', '.tiff'}
DOWNLOAD_QUEUE = config_root() / 'peek-downloads'
libc = ctypes.CDLL(None)
libc.wcwidth.argtypes = [ctypes.c_wchar]
libc.wcwidth.restype = ctypes.c_int


def clean(text):
    # Never interpret a cached message as terminal commands.
    return ''.join(c for c in str(text) if c in '\n\t' or (ord(c) >= 32 and not 127 <= ord(c) < 160)).expandtabs(4)


def cell_width(c):
    return max(0, libc.wcwidth(c))


def wrap(text, width):
    lines = []
    for paragraph in clean(text).split('\n'):
        line, size = '', 0
        for c in paragraph:
            n = cell_width(c)
            if size + n > width and line:
                lines.append(line)
                line, size = '', 0
            line += c
            size += n
        lines.append(line)
    return lines


def image_path(info):
    try:
        fields = info.strip().split(',')
        path = Path(bytes.fromhex(fields[2]).decode('utf-8'))
        if path.is_absolute() and path.is_file() and path.suffix.lower() in IMAGES:
            return str(path)
    except (IndexError, ValueError, UnicodeError, OSError):
        pass
    return None


def cached_file(info):
    try:
        path = Path(bytes.fromhex(info.strip().split(',')[2]).decode('utf-8'))
        return path if path.is_absolute() and path.is_file() else None
    except (IndexError, ValueError, UnicodeError, OSError):
        return None


def is_image_reference(info):
    try:
        fields = info.strip().split(',')
        path = bytes.fromhex(fields[2]).decode('utf-8')
        mime = bytes.fromhex(fields[3]).decode('utf-8')
        return (Path(path).suffix.lower() in IMAGES or Path(path).name.lower() in ('[photo]','[sticker]')
                or mime.startswith('image/'))
    except (IndexError, ValueError, UnicodeError):
        return False


def refresh_attachment(message):
    database = message.get('database')
    if not database:
        return False
    with contextlib.closing(open_cache(Path(database))) as db:
        row = db.execute('SELECT fileInfo FROM messages WHERE chatId=? AND id=?',
                         (message['chatId'], message['msgId'])).fetchone()
    if row and row[0]:
        message['fileInfo'] = row[0]
    return bool(cached_file(message.get('fileInfo','')))


def queue_download(message, queue=None):
    queue = DOWNLOAD_QUEUE if queue is None else Path(queue)
    for key in ('database', 'profileId', 'chatId', 'msgId'):
        if not message.get(key):
            raise ValueError('Reopen nchat-peek to enable downloads for this transcript.')
    try:
        st = queue.lstat()
        if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
            raise ValueError('Download queue permissions must be private (0700).')
        pid = int((queue/'server').read_text())
        os.kill(pid, 0)
    except (FileNotFoundError, ProcessLookupError):
        raise ValueError('Start the updated nchat with the same configuration directory, then press d again.') from None
    if refresh_attachment(message):
        return None
    if not is_image_reference(message.get('fileInfo','')):
        raise ValueError('This attachment is not identified as an image in the cache.')
    try:
        file_id = bytes.fromhex(message['fileInfo'].strip().split(',')[1])
    except (ValueError, IndexError):
        file_id = b''
    if not file_id:
        raise ValueError('No download reference is available for this attachment.')
    data = ''.join(str(message[key]).encode().hex()+'\n' for key in ('profileId','chatId','msgId'))
    if len(data) > 8192:
        raise ValueError('Attachment reference is too long.')
    request = queue/(secrets.token_hex(16)+'.request')
    fd, temporary = tempfile.mkstemp(prefix='.pending-', dir=queue)
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(data)
        os.replace(temporary, request)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return request


def dimensions(paths):
    sizes = {}
    if not shutil.which('sips'):
        return sizes
    paths = sorted(set(paths))
    original_paths = {str(Path(p).resolve()): p for p in paths}
    for start in range(0, len(paths), 64):
        result = subprocess.run(['/usr/bin/sips', '-g', 'pixelWidth', '-g', 'pixelHeight', '-g', 'orientation',
                                 *paths[start:start+64]], capture_output=True, text=True, timeout=30)
        current = None
        values = {}
        for line in result.stdout.splitlines():
            if line.startswith('/'):
                current = original_paths.get(str(Path(line.strip()).resolve()), line.strip())
                values[current] = {}
            elif current and ':' in line:
                key, value = line.strip().split(':', 1)
                if value.strip().isdigit():
                    values[current][key] = int(value)
        for path, data in values.items():
            w, h = data.get('pixelWidth', 0), data.get('pixelHeight', 0)
            if data.get('orientation', 1) in (5, 6, 7, 8):
                w, h = h, w
            if w > 0 and h > 0:
                sizes[path] = (w, h)
    return sizes


def image_box(size, columns, rows, cell_ratio=0.5):
    w, h = size
    cols = min(64, max(1, columns - 4))
    height = max(1, round(cols * cell_ratio * h / w))
    limit = max(1, min(32, rows - 5))
    if height > limit:
        height = limit
        cols = max(1, round(height / cell_ratio * w / h))
    return cols, height


def layout(messages, sizes, columns, rows, ratio):
    lines, placements = [], []
    for message in messages:
        message['_start'] = len(lines)
        lines.extend(wrap(message['text'].rstrip(), max(1, columns - 2)))
        path = image_path(message.get('fileInfo', ''))
        if path in sizes:
            cols, height = image_box(sizes[path], columns, rows, ratio)
            placements.append((len(lines), path, cols, height))
            lines.extend(['[image — scroll to bring it fully into view]'] + [''] * (height-1))
        elif message.get('fileInfo'):
            if cached_file(message['fileInfo']):
                lines.append('[downloaded attachment; this format cannot be previewed inline]')
            elif is_image_reference(message['fileInfo']):
                lines.append('[image not cached — press d to download without opening the chat]')
            else:
                lines.append('[attachment not cached; inline preview supports images]')
        if message.get('_download_state'):
            lines.extend(wrap(message['_download_state'], max(1,columns-2)))
        lines.append('')
        message['_end'] = len(lines)
    return lines, placements


def render_images(placements):
    if not THUMBNAILS:
        return
    args = [THUMBNAILS]
    for path, x, y, w, h in placements:
        args.extend((path, str(x), str(y), str(w), str(h)))
    subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=True)


def cell_ratio():
    try:
        rows, cols, xp, yp = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\0'*8))
        if min(rows, cols, xp, yp) > 0:
            return (xp / cols) / (yp / rows)
    except (OSError, ZeroDivisionError):
        pass
    return 0.5


def put(win, y, text, attr=0):
    # Leave the final column unused to prevent physical terminal autowrap.
    clipped = wrap(text, max(1, win.getmaxyx()[1]-1))[0]
    try:
        win.addstr(y, 0, clipped, attr)
    except curses.error:
        pass  # A resize may arrive between getmaxyx and addstr.


def viewer(win, title, messages, sizes):
    curses.curs_set(0)
    curses.start_color()
    curses.init_pair(1, curses.COLOR_WHITE, curses.COLOR_BLACK)
    win.bkgd(' ', curses.color_pair(1))
    win.keypad(True)
    win.timeout(50)
    rows, cols = win.getmaxyx()
    lines, images = layout(messages, sizes, cols, rows, cell_ratio())
    top = max(0, len(lines) - max(1, rows-2))
    query, note = '', ''
    dirty, shown, pending = True, False, []
    due = 0
    downloads = {}
    last_download_poll = 0
    while True:
        rows, cols = win.getmaxyx()
        page = max(1, rows-2)
        now = time.monotonic()
        if downloads and now-last_download_poll >= 0.5:
            last_download_poll = now
            reveal = None
            for index, (request, deadline) in list(downloads.items()):
                message = messages[index]
                try:
                    complete = refresh_attachment(message)
                except (OSError, sqlite3.Error):
                    complete = False
                if complete:
                    if message['_start'] < top+page and message['_end'] > top:
                        reveal = index
                    path = image_path(message['fileInfo'])
                    if path:
                        sizes.update(dimensions([path]))
                    message['_download_state'] = ''
                    del downloads[index]
                    dirty = True
                elif now >= deadline:
                    message['_download_state'] = 'Download timed out or media expired. Press d to retry.'
                    request.unlink(missing_ok=True)
                    del downloads[index]
                    dirty = True
            if dirty:
                lines, images = layout(messages, sizes, cols, rows, cell_ratio())
                if reveal is not None:
                    top = max(0, min(messages[reveal]['_start'], len(lines)-page))
        if dirty:
            render_images([])
            shown = False
            win.erase()
            put(win, 0, title, curses.A_REVERSE)
            pending = []
            top = max(0, min(top, max(0, len(lines)-page)))
            for y, line in enumerate(lines[top:top+page], 1):
                if y < rows-1:
                    put(win, y, line)
            for start, path, w, h in images:
                if top <= start and start+h <= top+page:
                    pending.append((path, 0, start-top+1, w, h))
                    put(win, start-top+1, ' ' * min(cols-1, w))
            put(win, rows-1, note or '↑↓/j k scroll | PgUp/PgDn | g/G | / search | n/N | d download | q exit', curses.A_REVERSE)
            win.refresh()
            due = time.monotonic() + 0.5
            dirty = False
        if not shown and pending and time.monotonic() >= due:
            render_images(pending)
            shown = True
        try:
            key = win.get_wch()
        except curses.error:
            continue
        note = ''
        if key in ('q', '\x03', '\x1b'):
            for request, _ in downloads.values():
                request.unlink(missing_ok=True)
            return
        if key == curses.KEY_RESIZE:
            at_end = top+page >= len(lines)
            lines, images = layout(messages, sizes, *reversed(win.getmaxyx()), cell_ratio())
            if at_end:
                top = max(0, len(lines)-max(1, win.getmaxyx()[0]-2))
        elif key in (curses.KEY_UP, 'k'):
            top -= 1
        elif key in (curses.KEY_DOWN, 'j', '\n'):
            top += 1
        elif key in (curses.KEY_PPAGE, 'b'):
            top -= page
        elif key in (curses.KEY_NPAGE, ' '):
            top += page
        elif key in ('g', curses.KEY_HOME):
            top = 0
        elif key in ('G', curses.KEY_END):
            top = len(lines)-page
        elif key == 'd':
            candidates = [(i,m) for i,m in enumerate(messages) if i not in downloads
                          and is_image_reference(m.get('fileInfo','')) and not cached_file(m['fileInfo'])
                          and m['_start'] < top+page and m['_end'] > top]
            if not candidates:
                note = 'No uncached image in view. Scroll to an image first.'
            else:
                index, message = candidates[0]
                try:
                    request = queue_download(message)
                    if request:
                        downloads[index] = (request,time.monotonic()+45)
                        message['_download_state'] = 'Downloading via nchat — you can keep scrolling.'
                    else:
                        path = image_path(message['fileInfo'])
                        if path:
                            sizes.update(dimensions([path]))
                    lines, images = layout(messages, sizes, cols, rows, cell_ratio())
                except (ValueError, OSError, sqlite3.Error) as error:
                    note = str(error)
        elif key == '/':
            render_images([])
            entered = ''
            while True:
                win.move(rows-1, 0)
                win.clrtoeol()
                put(win, rows-1, '/' + entered, curses.A_REVERSE)
                win.refresh()
                try:
                    ch = win.get_wch()
                except curses.error:
                    continue
                if ch in ('\n', '\r', curses.KEY_ENTER):
                    query = entered
                    key = 'n'
                    break
                if ch in ('\x1b', '\x03', curses.KEY_RESIZE):
                    if ch == curses.KEY_RESIZE:
                        lines, images = layout(messages, sizes, *reversed(win.getmaxyx()), cell_ratio())
                    break
                if ch in ('\x7f', '\b', curses.KEY_BACKSPACE):
                    entered = entered[:-1]
                elif isinstance(ch, str) and ch.isprintable():
                    entered += ch
        if key in ('n', 'N') and query:
            matches = [i for i, line in enumerate(lines) if query.casefold() in line.casefold()]
            candidates = [i for i in matches if (i > top if key == 'n' else i < top)]
            if matches:
                top = (candidates[0] if candidates else matches[0]) if key == 'n' else (candidates[-1] if candidates else matches[-1])
            else:
                note = 'No match: ' + query
        dirty = True


def open_cache(database):
    # Both the main cache and any contact database are opened read-only.
    db = sqlite3.connect(Path(database).resolve().as_uri()+'?mode=ro', uri=True, timeout=1)
    db.row_factory = sqlite3.Row
    return db


def setup_contacts(db, root, profile):
    fallback = 'CREATE TEMP VIEW resolved_contacts AS SELECT id,name FROM contacts2'
    session = root / 'profiles' / profile / 'session.db'
    if profile.startswith('WhatsAppMd_') and session.is_file():
        try:
            db.execute('ATTACH DATABASE ? AS wa', (session.resolve().as_uri()+'?mode=ro',))
            db.execute("""
                CREATE TEMP VIEW resolved_contacts AS
                WITH candidates AS (
                  SELECT id,name,0 AS priority FROM contacts2
                  WHERE trim(COALESCE(name,'')) <> '' AND name <> id
                    AND name <> replace(id,'@s.whatsapp.net','')
                    AND name <> replace(id,'@lid','')
                  UNION ALL
                  SELECT their_jid,
                    COALESCE(NULLIF(full_name,''),NULLIF(first_name,''),
                             NULLIF(push_name,''),NULLIF(business_name,'')),1
                  FROM wa.whatsmeow_contacts
                ), aliases AS (
                  SELECT lid||'@lid' AS id,pn||'@s.whatsapp.net' AS other
                  FROM wa.whatsmeow_lid_map
                  UNION ALL
                  SELECT pn||'@s.whatsapp.net',lid||'@lid'
                  FROM wa.whatsmeow_lid_map
                ), expanded AS (
                  SELECT id,name,priority FROM candidates
                  UNION ALL
                  SELECT a.id,c.name,c.priority+2 FROM aliases a
                  JOIN candidates c ON c.id=a.other
                ), ranked AS (
                  SELECT id,name,row_number() OVER (PARTITION BY id ORDER BY priority,name) AS rn
                  FROM expanded WHERE trim(COALESCE(name,'')) <> ''
                ) SELECT id,name FROM ranked WHERE rn=1
            """)
            db.execute('SELECT * FROM resolved_contacts LIMIT 1').fetchone()
            return
        except sqlite3.Error:
            db.execute('DROP VIEW IF EXISTS temp.resolved_contacts')
    db.execute(fallback)


def local_time(milliseconds, fmt):
    try:
        return datetime.fromtimestamp(int(milliseconds)/1000).strftime(fmt)
    except (OSError, OverflowError, TypeError, ValueError):
        return 'Unknown time'


def read_chats(root):
    chats = []
    for database in sorted((root / 'history').glob('*/db.sqlite')):
        try:
            with contextlib.closing(open_cache(database)) as db:
                profile = database.parent.name
                setup_contacts(db, root, profile)
                rows = db.execute("""
                    SELECT ch.id,COALESCE(NULLIF(c.name,''),ch.id) AS name,
                           COALESCE(latest.timeSent,0) AS timeSent
                    FROM chats2 ch LEFT JOIN resolved_contacts c ON c.id=ch.id
                    LEFT JOIN (
                      SELECT chatId,MAX(CAST(timeSent AS INTEGER)) AS timeSent
                      FROM messages GROUP BY chatId
                    ) latest ON latest.chatId=ch.id
                """)
                for row in rows:
                    chats.append(dict(database=str(database.resolve()),profileId=profile,
                                      service=profile.split('_',1)[0],chatId=row['id'],
                                      name=row['name'],timeSent=int(row['timeSent'] or 0)))
        except sqlite3.Error as error:
            print('Could not read a conversation cache: '+str(error),file=sys.stderr)
    return sorted(chats,key=lambda chat: chat['timeSent'],reverse=True)


def read_messages(chat, root):
    messages = []
    with contextlib.closing(open_cache(Path(chat['database']))) as db:
        setup_contacts(db,root,chat['profileId'])
        columns = {row['name'] for row in db.execute('PRAGMA table_info(messages)')}
        # Old upstream caches lack these flags; never migrate a cache while browsing.
        flags = ','.join(('COALESCE(m.'+name+',0)' if name in columns else '0')+' AS '+name
                         for name in ('isEdited','isDeleted'))
        rows = db.execute("""
            SELECT m.id,m.senderId,COALESCE(m.text,'') AS text,
                   COALESCE(m.quotedText,'') AS quotedText,COALESCE(m.fileInfo,'') AS fileInfo,
                   m.timeSent,m.isOutgoing,COALESCE(NULLIF(sender.name,''),m.senderId,'Unknown') AS sender,
                   """+flags+"""
            FROM messages m LEFT JOIN resolved_contacts sender ON sender.id=m.senderId
            WHERE m.chatId=? ORDER BY CAST(m.timeSent AS INTEGER),m.id
        """,(chat['chatId'],))
        for row in rows:
            sender = 'You' if row['isOutgoing'] else row['sender']
            text = '['+local_time(row['timeSent'],'%Y-%m-%d %H:%M:%S')+'] '+sender
            if row['isEdited']:
                text += ' (edited)'
            text += '\n'
            if row['quotedText']:
                text += '  > '+row['quotedText'].replace('\n','\n  > ')+'\n'
            text += row['text'] or ('[attachment]' if row['fileInfo'] else '[empty message]')
            if row['isDeleted']:
                text += ' [deleted]'
            messages.append(dict(text=text+'\n',fileInfo=row['fileInfo'],
                                 database=chat['database'],profileId=chat['profileId'],
                                 chatId=chat['chatId'],msgId=row['id']))
    return messages


def pick_chat(root, query):
    if not shutil.which('fzf'):
        raise ValueError('Install fzf to choose a cached conversation.')
    chats = read_chats(root)
    if not chats:
        raise ValueError('No cached nchat conversations found in '+str(root))
    listing = []
    for index, chat in enumerate(chats):
        name = clean(chat['name']).replace('\t',' ').replace('\n',' ')
        date = local_time(chat['timeSent'],'%Y-%m-%d %H:%M:%S') if chat['timeSent'] else 'No cached messages'
        listing.append(str(index)+'\t'+chat['service']+'\t'+name+'\t'+date)
    result = subprocess.run(
        ['fzf','--height=85%','--reverse','--border','--no-sort',
         '--delimiter=\t','--with-nth=2,3,4','--prompt=Chat > ',
         '--header=Type to search  |  Enter: read  |  Esc: cancel','--query='+query],
        input='\n'.join(listing),text=True,stdout=subprocess.PIPE)
    if result.returncode in (1,130):
        return None
    if result.returncode:
        raise ValueError('The conversation picker failed.')
    return chats[int(result.stdout.split('\t',1)[0])]


def self_test():
    assert config_root('/tmp/nchat-example') == Path('/tmp/nchat-example').resolve()
    assert config_root(environ={'HOME':'/tmp/nchat-peek-nonexistent-home',
                               'XDG_CONFIG_HOME':'/tmp/nchat-xdg'}) == Path('/tmp/nchat-xdg/nchat').resolve()
    assert config_root(environ={'NCHAT_CONFIG_DIR':'/tmp/nchat-custom'}) == Path('/tmp/nchat-custom').resolve()
    assert clean('a\x1b[31m\x07b') == 'a[31mb'
    assert wrap('abcdef',3) == ['abc','def']
    assert image_box((100,100),80,24) == (38,19)
    with tempfile.TemporaryDirectory(prefix='nchat-peek-test-') as directory:
        root = Path(directory).resolve()
        fake_home = root / 'home'
        fake_home.mkdir()
        env = {'HOME':str(fake_home),'XDG_CONFIG_HOME':str(root / 'xdg')}
        assert config_root(environ=env) == root / 'xdg' / 'nchat'
        (fake_home / '.nchat').mkdir()
        assert config_root(environ=env) == fake_home / '.nchat'
        assert config_root(str(root / 'explicit'),env) == root / 'explicit'
        assert config_root(environ={**env,'NCHAT_CONFIG_DIR':str(root / 'env')}) == root / 'env'
        profile = 'WhatsAppMd_test'
        database = root / 'history' / profile / 'db.sqlite'
        database.parent.mkdir(parents=True)
        with sqlite3.connect(database) as db:
            # Current logical cache schema, deliberately without obsolete fileType.
            db.executescript("""
                CREATE TABLE chats2(id TEXT,isMuted INT,isPinned INT,lastMessageTime INT,isArchived INT);
                CREATE TABLE contacts2(id TEXT,name TEXT,phone TEXT,isSelf INT,isAlias INT);
                CREATE TABLE messages(chatId TEXT,id TEXT,senderId TEXT,text TEXT,quotedId TEXT,
                    quotedText TEXT,quotedSender TEXT,fileInfo TEXT,timeSent INT,isOutgoing INT,
                    isRead INT,isEdited INT,isDeleted INT,isPinned INT,reactions BLOB,isSequence INT);
                INSERT INTO chats2(id) VALUES ('chat');
                INSERT INTO contacts2(id,name) VALUES ('chat','Sample group'),('100@lid','100');
            """)
            info = '0,'+b'image-id'.hex()+','+b'/missing-image.png'.hex()+','+b'image/png'.hex()
            db.execute('INSERT INTO messages(chatId,id,senderId,text,quotedText,fileInfo,timeSent,'
                       'isOutgoing,isEdited,isDeleted) VALUES (?,?,?,?,?,?,?,?,?,?)',
                       ('chat','message','100@lid','Synthetic message','A quote',info,1000000,0,1,1))
        session = root / 'profiles' / profile / 'session.db'
        session.parent.mkdir(parents=True)
        with sqlite3.connect(session) as db:
            db.executescript("""
                CREATE TABLE whatsmeow_contacts(their_jid TEXT,full_name TEXT,first_name TEXT,
                    push_name TEXT,business_name TEXT);
                CREATE TABLE whatsmeow_lid_map(lid TEXT,pn TEXT);
                INSERT INTO whatsmeow_contacts VALUES ('200@s.whatsapp.net','Sample Sender','','','');
                INSERT INTO whatsmeow_lid_map VALUES ('100','200');
            """)
        chats = read_chats(root)
        assert len(chats) == 1 and chats[0]['name'] == 'Sample group'
        messages = read_messages(chats[0],root)
        assert len(messages) == 1 and 'Sample Sender' in messages[0]['text']
        assert '(edited)' in messages[0]['text'] and '[deleted]' in messages[0]['text']
        # Bound ids remain safe, and older caches work without custom columns.
        legacy = root / 'history' / 'Telegram_test' / 'db.sqlite'
        legacy.parent.mkdir(parents=True)
        with sqlite3.connect(legacy) as db:
            db.executescript("""
                CREATE TABLE chats2(id TEXT);
                CREATE TABLE contacts2(id TEXT,name TEXT);
                CREATE TABLE messages(chatId TEXT,id TEXT,senderId TEXT,text TEXT,
                    quotedText TEXT,fileInfo TEXT,timeSent INT,isOutgoing INT);
            """)
            db.execute('INSERT INTO chats2 VALUES (?)',("chat'quoted",))
            db.execute('INSERT INTO contacts2 VALUES (?,?)',("chat'quoted",'Old group'))
            db.execute('INSERT INTO messages VALUES (?,?,?,?,?,?,?,?)',
                       ("chat'quoted",'old','sender','Legacy text','','',2000000,1))
        legacy_chat = next(chat for chat in read_chats(root) if chat['profileId'] == 'Telegram_test')
        assert 'You' in read_messages(legacy_chat,root)[0]['text']
        assert is_image_reference(info) and not cached_file(info)
        assert not refresh_attachment(messages[0])
        with contextlib.closing(open_cache(database)) as db:
            try:
                db.execute('DELETE FROM messages')
            except sqlite3.OperationalError:
                pass
            else:
                raise AssertionError('Cache must be read-only')
        queue = root / 'queue'
        queue.mkdir(mode=0o700)
        (queue / 'server').write_text(str(os.getpid()))
        request = queue_download(messages[0],queue)
        assert request.read_text().splitlines() == [messages[0][key].encode().hex()
                                                  for key in ('profileId','chatId','msgId')]
        assert stat.S_IMODE(request.stat().st_mode) == 0o600
        queue.chmod(0o755)
        try:
            queue_download(messages[0],queue)
        except ValueError as error:
            assert 'private' in str(error)
        else:
            raise AssertionError('An unprotected queue must be rejected')
    print('nchat-peek self-test passed (synthetic data only)')


def main():
    global DOWNLOAD_QUEUE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config-dir','--config','--confdir','-d',dest='config_dir',
                        help='nchat config directory (default: NCHAT_CONFIG_DIR, legacy ~/.nchat, or XDG_CONFIG_HOME/nchat)')
    parser.add_argument('--pick',action='store_true',help='choose a conversation with fzf')
    parser.add_argument('--self-test',action='store_true',help='test synthetic data without opening your cache')
    parser.add_argument('inputs',nargs='*',help='chat search words, or an existing transcript')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    root = config_root(args.config_dir)
    DOWNLOAD_QUEUE = root / 'peek-downloads'
    if args.pick:
        chat = pick_chat(root,' '.join(args.inputs))
        if chat is None:
            return
        title = clean(chat['name'])+' ('+chat['service']+') — cached copy, read-only'
        messages = read_messages(chat,root)
    else:
        if len(args.inputs) != 1:
            parser.error('provide a transcript or use --pick')
        with open(args.inputs[0]) as f:
            title = clean(next(f).strip())
            next(f)  # previous pager instructions
            next(f)  # blank line
            messages = [json.loads(line) for line in f if line.strip()]
    print('Loading cached images…', flush=True)
    sizes = dimensions([path for m in messages if (path := image_path(m.get('fileInfo', '')))])
    try:
        curses.wrapper(viewer, title, messages, sizes)
    finally:
        render_images([])


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as error:
        print('nchat-peek: '+str(error),file=sys.stderr)
        sys.exit(1)
