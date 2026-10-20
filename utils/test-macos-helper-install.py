#!/usr/bin/env python3
"""Offline installer checks; every destination and source is a temporary fixture."""
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        info = path.lstat()
        data = os.readlink(path) if path.is_symlink() else path.read_bytes() if path.is_file() else None
        result[str(path.relative_to(root))] = (stat.S_IMODE(info.st_mode), info.st_mtime_ns, data)
    return result


def main():
    installer = Path(__file__).with_name("install-macos-helpers.sh")
    with tempfile.TemporaryDirectory(prefix="nchat-helper-test-") as temporary:
        # macOS /var is itself a system symlink; test targets are canonical paths.
        root = Path(temporary).resolve()
        source = root / "source/utils"
        source.mkdir(parents=True)
        script = source / installer.name
        shutil.copyfile(installer, script)
        for name in ("nchat-image", "nchat-thumbnails", "nchat-image-clear", "nchat-peek",
                     "nchat-peek-view.py", "nchat_badge.py", "nchat_badge_auth.py", "kitty-nchat.conf"):
            (source / name).write_text("fixture version one\n")
        for name in ("nchat Unread.app", "nchat-transcribe.app", "nchat-record.app"):
            binary = root / "source/build" / name / "Contents/MacOS/fixture"
            binary.parent.mkdir(parents=True)
            binary.write_text("app fixture version one\n")
            binary.chmod(0o755)
        env = dict(os.environ,
                   NCHAT_HELPER_PREFIX=str(root / "installed/prefix"),
                   NCHAT_CONFIG_DIR=str(root / "installed/config"),
                   NCHAT_KITTY_CONFIG_DIR=str(root / "installed/kitty"),
                   NCHAT_APPLICATIONS_DIR=str(root / "installed/Applications"))
        prefix = Path(env["NCHAT_HELPER_PREFIX"])
        state = prefix / ".nchat-helper-install"
        manifest = state / "installed.json"
        image = prefix / "bin/nchat-image"
        app = Path(env["NCHAT_APPLICATIONS_DIR"]) / "nchat Unread.app/Contents/MacOS/fixture"

        def run(*arguments, success=True):
            result = subprocess.run(["sh", str(script), *arguments], env=env, text=True, capture_output=True)
            assert (result.returncode == 0) == success, result.stdout + result.stderr
            return result.stdout + result.stderr

        before = snapshot(root)
        assert "Would install" in run("--dry-run")
        assert snapshot(root) == before, "fresh dry-run wrote files"
        assert "Installed" in run()
        assert len(json.loads(manifest.read_text())) == 11
        assert image.stat().st_mode & 0o777 == 0o755
        assert app.read_text() == "app fixture version one\n"
        assert state.stat().st_mode & 0o777 == 0o700
        assert manifest.stat().st_mode & 0o777 == 0o600
        before = snapshot(root)
        assert "Kept existing" in run()
        assert snapshot(root) == before, "default repeat install modified files"
        assert "Up to date" in run("--upgrade")
        assert snapshot(root) == before, "unchanged managed upgrade modified files"

        (source / "nchat-image").write_text("fixture version two\n")
        source_app = root / "source/build/nchat Unread.app/Contents/MacOS/fixture"
        source_app.write_text("app fixture version two\n")
        before = snapshot(root)
        assert "Would upgrade" in run("--upgrade", "--dry-run")
        assert snapshot(root) == before, "upgrade dry-run wrote files"
        assert "Upgraded" in run("--upgrade")
        assert image.read_text() == "fixture version two\n"
        assert app.read_text() == "app fixture version two\n"
        backups = list((state / "backups").iterdir())
        assert len(backups) == 2
        assert all(path.stat().st_mode & 0o777 == 0o700 for path in backups)
        assert any((path / "nchat-image").is_file() and (path / "nchat-image").read_text() == "fixture version one\n"
                   for path in backups)
        assert any((path / "nchat Unread.app/Contents/MacOS/fixture").is_file()
                   and (path / "nchat Unread.app/Contents/MacOS/fixture").read_text() == "app fixture version one\n"
                   for path in backups)

        image.write_text("my customized helper\n")
        before = snapshot(root)
        output = run("--upgrade", success=False)
        assert "--replace-reviewed 'nchat-image'" in output
        assert snapshot(root) == before, "refused upgrade partially modified installation"
        assert "Kept existing" in run()
        assert snapshot(root) == before
        assert "private backup first" in run("--upgrade", "--replace-reviewed", "nchat-image", "--dry-run")
        assert snapshot(root) == before
        run("--upgrade", "--replace-reviewed", "nchat-image")
        assert image.read_text() == "fixture version two\n"
        assert any((path / "nchat-image").is_file() and (path / "nchat-image").read_text() == "my customized helper\n"
                   for path in (state / "backups").iterdir())

        records = json.loads(manifest.read_text())
        records.pop(str(image))
        manifest.write_text(json.dumps(records))
        before = snapshot(root)
        assert "Up to date" in run("--upgrade")
        assert snapshot(root) == before, "matching unmanaged helper should be an unchanged no-op"
        image.write_text("unmanaged helper\n")
        before = snapshot(root)
        assert "unmanaged target" in run("--upgrade", success=False)
        assert snapshot(root) == before
        image.write_text("fixture version two\n")
        assert "Adopted reviewed target" in run("--upgrade", "--replace-reviewed", "nchat-image")
        assert str(image) in json.loads(manifest.read_text())

        before = snapshot(root)
        assert "requires --upgrade" in run("--replace-reviewed", "nchat-image", success=False)
        assert "unknown destination basename" in run("--upgrade", "--replace-reviewed", "../nchat-image", success=False)
        assert "unknown destination basename" in run("--only", "not-a-helper", success=False)
        assert snapshot(root) == before
        sentinel = root / "outside-helper"
        sentinel.write_text("do not change\n")
        image.unlink()
        image.symlink_to(sentinel)
        before = snapshot(root)
        assert "Refusing symlink" in run("--upgrade", "--replace-reviewed", "nchat-image", success=False)
        assert snapshot(root) == before
        image.unlink()
        image.write_text("fixture version two\n")
        image.chmod(0o755)
        (app.parent / "link").symlink_to(sentinel)
        before = snapshot(root)
        assert "Up to date" in run("--upgrade", "--only", "nchat-image", "--only", "nchat-peek")
        assert snapshot(root) == before, "scoped upgrade touched an unselected bundle"
        assert "Refusing symlink" in run("--upgrade", "--replace-reviewed", "nchat Unread.app", success=False)
        assert snapshot(root) == before
        (source / "nchat_badge.py").unlink()
        before = snapshot(root)
        assert "Up to date" in run("--upgrade", "--only", "nchat-image")
        assert snapshot(root) == before, "unselected missing source affected a scoped run"
        assert "Missing bundled helper" in run("--only", "nchat_badge.py", success=False)
        assert snapshot(root) == before
        assert sentinel.read_text() == "do not change\n"
    print("PASS: fresh/no-op/scoped managed upgrade, private backups, reviewed replacement, no-write dry-run and symlink refusal")


if __name__ == "__main__":
    main()
