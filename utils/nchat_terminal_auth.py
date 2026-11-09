"""Allow nchat's badge and bounded emoji painting in the requesting window."""
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parent
BADGE = runpy.run_path(str(ROOT / 'nchat_badge_auth.py'))['is_cmd_allowed']
DECODE = runpy.run_path(str(ROOT / 'nchat_emoji.py'))['decode']
EMOJI_HELPER = str(ROOT / 'nchat_emoji.py')


def is_cmd_allowed(command, window, from_socket, extra_data):
    if BADGE(command, window, from_socket, extra_data):
        return True
    if not isinstance(command, dict) or from_socket or window is None:
        return False
    payload = command.get('payload', {})
    if (command.get('cmd') != 'kitten' or command.get('no_response') is not True or
            not isinstance(payload, dict) or set(payload) != {'kitten', 'args'} or
            payload.get('kitten') != EMOJI_HELPER):
        return False
    args = payload.get('args')
    return isinstance(args, list) and len(args) == 1 and DECODE(args[0]) is not None
