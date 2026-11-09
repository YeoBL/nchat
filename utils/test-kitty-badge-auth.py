"""Check the narrow badge permission without issuing any remote commands."""
import copy
from pathlib import Path
import runpy

helpers = Path(__file__).resolve().parent
allow = runpy.run_path(str(helpers / 'nchat_badge_auth.py'))['is_cmd_allowed']
valid = {'cmd': 'kitten', 'no_response': True,
         'payload': {'kitten': str(helpers / 'nchat_badge.py'), 'args': ['1', '123:456']}}
assert allow(valid, None, False, {})
for state in ('0', '1'):
    cmd = copy.deepcopy(valid)
    cmd['payload']['args'][0] = state
    assert allow(cmd, None, False, {})
assert not allow(valid, None, True, {}) # No socket access.
for action in ('launch', 'send-text', 'focus-window', 'close-window', 'load-config-file'):
    cmd = copy.deepcopy(valid)
    cmd['cmd'] = action
    assert not allow(cmd, None, False, {})
for args in (['enable'], ['1'], ['2', '123:456'], ['1', '../path'], ['1', '1:2', 'extra'], None):
    cmd = copy.deepcopy(valid)
    cmd['payload']['args'] = args
    assert not allow(cmd, None, False, {})
cmd = copy.deepcopy(valid)
cmd['payload']['kitten'] = '/tmp/arbitrary.py'
assert not allow(cmd, None, False, {})
cmd = copy.deepcopy(valid)
cmd['payload']['match'] = 'all'
assert not allow(cmd, None, False, {})
cmd = copy.deepcopy(valid)
cmd['no_response'] = False
assert not allow(cmd, None, False, {})
cmd['payload'] = []
assert not allow(cmd, None, False, {})
assert not allow(None, None, False, {})
print('Kitty badge authorization: only fixed badge on/off allowed; broader control denied')

# The additional renderer can only paint bounded Unicode cells in the sender's
# own window. It cannot launch processes, inject input or target other windows.
import json
allow = runpy.run_path(str(helpers / 'nchat_terminal_auth.py'))['is_cmd_allowed']
assert allow(valid, None, False, {})
row = [1, 2, '🙂‍↔️', 2, -1, -1, False, False, False, False]
render = {'cmd': 'kitten', 'no_response': True,
          'payload': {'kitten': str(helpers / 'nchat_emoji.py'), 'args': [json.dumps([row])]}}
assert allow(render, object(), False, {})
assert not allow(render, None, False, {})
assert not allow(render, object(), True, {})
for index, value in ((0, -1), (1, 8192), (2, '\x1b[2J'), (2, '\u202e'), (2, 'x' * 65),
                     (3, 3), (3, True), (4, 256), (6, 1)):
    bad = row.copy(); bad[index] = value
    command = copy.deepcopy(render)
    command['payload']['args'] = [json.dumps([bad])]
    assert not allow(command, object(), False, {})
for bad in ('null', '{}', 'invalid', json.dumps([row] * 2049)):
    command = copy.deepcopy(render); command['payload']['args'] = [bad]
    assert not allow(command, object(), False, {})
command = copy.deepcopy(render); command['payload']['match'] = 'all'
assert not allow(command, object(), False, {})
print('Kitty emoji authorization: only bounded in-window glyph painting allowed; input, execution and cross-window targeting denied')
