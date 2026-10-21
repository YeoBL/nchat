#!/usr/bin/env python3
"""Run with python3 utils/test-nchat-image.py. No native app or media is opened."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    source = Path(__file__).with_name('nchat-image').read_text()
    with tempfile.TemporaryDirectory(prefix='nchat-image-routing.') as directory:
        root = Path(directory)
        binary = root / 'bin'
        binary.mkdir()
        log = root / 'calls.jsonl'
        stub = f'''#!{sys.executable}
import json, os, sys
from pathlib import Path
with open(os.environ['NCHAT_TEST_LOG'], 'a') as output:
    output.write(json.dumps([Path(sys.argv[0]).name, *sys.argv[1:]]) + '\\n')
if Path(sys.argv[0]).name == 'file':
    if os.environ.get('NCHAT_TEST_MIME_FAILURE'):
        sys.exit(1)
    print(os.environ['NCHAT_TEST_MIME'])
'''
        for name in ('file', 'open', 'qlmanage', 'chafa'):
            path = binary / name
            path.write_text(stub)
            path.chmod(0o700)
        # Only executable locations and the terminal output sink are substituted.
        # Branches, quoting, and command arguments remain the production helper.
        helper = source
        for name in ('file', 'open', 'qlmanage'):
            assert helper.count('/usr/bin/' + name) == 1
            helper = helper.replace('/usr/bin/' + name, shlex.quote(str(binary / name)))
        helper = helper.replace('render_tty=/dev/tty', 'render_tty=' + shlex.quote(str(root / 'terminal')))
        script = root / 'nchat-image'
        script.write_text(helper)
        synthetic = root / "synthetic ' $(touch unexpected) `touch unexpected2`.mp4"
        synthetic.write_bytes(b'Synthetic routing fixture; never parsed as media')
        env = {key: value for key, value in os.environ.items() if key not in ('STY', 'ZDOTDIR')}
        env.update(PATH=str(binary), NCHAT_TEST_LOG=str(log), NCHAT_TEST_MIME='video/mp4')

        def run(mime, *, exists=True):
            log.unlink(missing_ok=True)
            env['NCHAT_TEST_MIME'] = mime
            result = subprocess.run(['/bin/zsh', '-f', str(script), str(synthetic if exists else root / 'missing')],
                                    env=env, cwd=root, input='', text=True, capture_output=True, timeout=5)
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            assert not (root / 'unexpected').exists() and not (root / 'unexpected2').exists()
            return result, calls

        for mime in ('video/mp4', 'video/quicktime'):
            result, calls = run(mime)
            assert result.returncode == 0, result.stderr
            assert calls == [['file', '-b', '--mime-type', '--', str(synthetic)],
                             ['open', '-b', 'com.apple.QuickTimePlayerX', '--', str(synthetic)]], calls

        for mime in ('audio/ogg', 'audio/mpeg', 'application/pdf', 'text/plain', 'application/octet-stream'):
            result, calls = run(mime)
            assert result.returncode == 0, result.stderr
            assert calls[-1] == ['qlmanage', '-p', str(synthetic)], calls
            assert not any(call[0] == 'open' for call in calls)
        env['NCHAT_TEST_MIME_FAILURE'] = '1'
        result, calls = run('')
        assert result.returncode == 0 and calls[-1] == ['qlmanage', '-p', str(synthetic)], calls
        del env['NCHAT_TEST_MIME_FAILURE']

        result, calls = run('image/png')
        assert result.returncode == 0, result.stderr
        assert any(call[0] == 'chafa' and call[-1] == str(synthetic) for call in calls), calls
        assert not any(call[0] in ('open', 'qlmanage') for call in calls), calls

        (binary / 'chafa').unlink()
        result, calls = run('image/jpeg')
        assert result.returncode == 0, result.stderr
        assert calls[-1] == ['qlmanage', '-p', str(synthetic)], calls
        result, calls = run('video/mp4', exists=False)
        assert result.returncode != 0 and 'File not found:' in result.stderr and not calls
        print('PASS: videos use QuickTime; audio/documents and missing-chafa images retain Quick Look; '
              'terminal images, missing files and quoted paths verified. No native apps/media opened.')


if __name__ == '__main__':
    main()
