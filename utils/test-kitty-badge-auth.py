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
