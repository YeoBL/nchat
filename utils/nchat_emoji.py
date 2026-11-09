"""Paint complete emoji and combining accents in the requesting Kitty window."""
import json


def decode(value):
    if not isinstance(value, str) or len(value) > 262144:
        return None
    try:
        rows = json.loads(value)
    except (ValueError, RecursionError):
        return None
    if not isinstance(rows, list) or len(rows) > 2048:
        return None
    for row in rows:
        if not isinstance(row, list) or len(row) != 10:
            return None
        y, x, text, width, fg, bg, *attributes = row
        if (type(y) is not int or type(x) is not int or not 0 <= y < 2048 or not 0 <= x < 8192 or
                type(width) is not int or width not in (1, 2) or type(fg) is not int or type(bg) is not int or not -1 <= fg < 256 or not -1 <= bg < 256 or
                any(type(flag) is not bool for flag in attributes) or
                not isinstance(text, str) or not 1 <= len(text) <= 64):
            return None
        if any(ord(char) < 32 or 0x7f <= ord(char) <= 0x9f or 0xd800 <= ord(char) <= 0xdfff or
               0x202a <= ord(char) <= 0x202e or 0x2066 <= ord(char) <= 0x2069 for char in text):
            return None
    return rows


def paint(screen, rows):
    from kitty.fast_data_types import wcswidth
    cursor = screen.cursor
    previous = cursor.copy()
    try:
        for y, x, text, width, fg, bg, bold, dim, reverse, underline in rows:
            if y >= screen.lines or x + width > screen.columns or wcswidth(text) != width:
                continue
            screen.cursor_position(y + 1, x + 1)
            cursor.reset_display_attrs()
            cursor.fg = 0 if fg < 0 else (fg << 8) | 1
            cursor.bg = 0 if bg < 0 else (bg << 8) | 1
            cursor.bold, cursor.dim, cursor.reverse = bold, dim, reverse
            cursor.decoration = int(underline)
            screen.draw(text)
    finally:
        for name in ('x', 'y', 'fg', 'bg', 'bold', 'dim', 'reverse', 'decoration',
                     'decoration_fg', 'italic', 'blink', 'strikethrough', 'shape'):
            setattr(cursor, name, getattr(previous, name))
        screen.select_graphic_rendition(5 if previous.text_blink else 25)


def main(args):
    pass


# Validation also runs in the ordinary Python authorization regression test.
try:
    from kittens.tui.handler import result_handler
except ImportError:
    pass
else:
    @result_handler(no_ui=True)
    def handle_result(args, answer, target_window_id, boss):
        if len(args) != 2:
            return
        rows = decode(args[1])
        window = boss.window_id_map.get(target_window_id)
        if rows is not None and window is not None:
            paint(window.screen, rows)
