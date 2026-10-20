#!/bin/sh
# Preserve existing installations unless an upgrade is explicitly requested.
set -eu
[ "$(uname -s)" = Darwin ] || { printf '%s\n' 'These helpers require macOS.' >&2; exit 1; }
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 - "$source_dir" "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

parser = argparse.ArgumentParser(prog="install-macos-helpers.sh",
                                 description="Install nchat macOS helpers without overwriting custom files.")
parser.add_argument("--dry-run", action="store_true", help="report actions without writing anything")
parser.add_argument("--upgrade", action="store_true", help="upgrade unchanged installer-managed targets")
parser.add_argument("--only", action="append", default=[], metavar="NAME",
                    help="process only this exact destination basename; repeatable")
parser.add_argument("--replace-reviewed", action="append", default=[], metavar="NAME",
                    help="with --upgrade, replace this exact reviewed basename, keeping a private backup; repeatable")
source_dir = Path(sys.argv.pop(1))
args = parser.parse_args()
if args.replace_reviewed and not args.upgrade:
    parser.error("--replace-reviewed requires --upgrade")

home = Path.home()
xdg = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config")
prefix = Path(os.environ.get("NCHAT_HELPER_PREFIX") or home / ".local").absolute()
kitty = Path(os.environ.get("NCHAT_KITTY_CONFIG_DIR") or xdg / "kitty").absolute()
config = Path(os.environ.get("NCHAT_CONFIG_DIR") or (home / ".nchat" if (home / ".nchat").is_dir()
                                                   else xdg / "nchat")).absolute()
system_app = Path("/Applications/nchat Unread.app")
applications = Path(os.environ.get("NCHAT_APPLICATIONS_DIR") or (system_app.parent if os.path.lexists(system_app)
                                                               else home / "Applications")).absolute()
state = prefix / ".nchat-helper-install"
manifest = state / "installed.json"
backups = state / "backups"
targets = [(source_dir / name, prefix / "bin" / name, 0o755)
           for name in ("nchat-image", "nchat-thumbnails", "nchat-image-clear", "nchat-peek")]
targets += [(source_dir / "nchat-peek-view.py", prefix / "lib/nchat-peek-view.py", 0o644)]
targets += [(source_dir / name, kitty / name, 0o644) for name in ("nchat_badge.py", "nchat_badge_auth.py")]
targets += [(source_dir / "kitty-nchat.conf", kitty / "nchat-example.conf", 0o644)]
targets += [(source_dir.parent / "build" / name,
             (applications if name == "nchat Unread.app" else config / "helpers") / name, None)
            for name in ("nchat Unread.app", "nchat-transcribe.app", "nchat-record.app")]
names = {destination.name for _, destination, _ in targets}
unknown = (set(args.replace_reviewed) | set(args.only)) - names
if unknown:
    parser.error("unknown destination basename: " + ", ".join(sorted(unknown)))
if args.only:
    targets = [target for target in targets if target[1].name in args.only]


def no_symlinks(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError(f"Refusing symlink: {part}. Choose a real installation path instead.")


def digest(path, mode=None):
    """Content, relative paths and permissions; app metadata/xattrs are not receipts."""
    h = hashlib.sha256()
    entries = [path] + sorted(path.rglob("*")) if path.is_dir() else [path]
    for entry in entries:
        info = entry.lstat()
        if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
            raise ValueError(f"Refusing symlink or special file: {entry}")
        label = "." if entry == path else str(entry.relative_to(path))
        permissions = mode if entry == path and mode is not None else stat.S_IMODE(info.st_mode)
        h.update(json.dumps([label, stat.S_ISDIR(info.st_mode), permissions,
                             info.st_size if stat.S_ISREG(info.st_mode) else 0]).encode() + b"\0")
        if entry.is_file():
            with entry.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    h.update(block)
    return h.hexdigest()


def private_directory(path):
    no_symlinks(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir() or stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError(f"Private installer directory must have mode 700: {path}")


def copy_target(source, destination, mode):
    if mode is None:
        subprocess.run(["/usr/bin/ditto", str(source), str(destination)], check=True)
    else:
        shutil.copyfile(source, destination)
        destination.chmod(mode)


def save_records(records):
    private_directory(state)
    descriptor, temporary = tempfile.mkstemp(prefix=".installed-", dir=state)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(records, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary, manifest)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


try:
    for path in (state, manifest, backups):
        no_symlinks(path)
    for directory in (state, backups):
        if directory.exists() and (not directory.is_dir() or stat.S_IMODE(directory.stat().st_mode) & 0o077):
            raise ValueError(f"Private installer directory must have mode 700: {directory}")
    records = json.loads(manifest.read_text()) if manifest.exists() else {}
    if not isinstance(records, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                            for k, v in records.items()):
        raise ValueError(f"Invalid installer manifest: {manifest}; restore it before upgrading.")
    plans = []
    errors = []
    for source, destination, mode in targets:
        try:
            no_symlinks(destination)
            if not source.exists() or (mode is None and not source.is_dir()):
                if mode is None:
                    print(f"Not built yet: {source.name}")
                    continue
                raise ValueError(f"Missing bundled helper: {source}")
            desired = digest(source, mode)
            if destination.exists():
                actual = digest(destination)
                if not args.upgrade:
                    print(f"Kept existing: {destination}")
                    continue
                reviewed = destination.name in args.replace_reviewed
                if actual == desired:
                    print(f"Up to date: {destination}")
                    # An explicitly reviewed matching target can become managed.
                    if reviewed and records.get(str(destination)) != actual:
                        plans.append((source, destination, mode, desired, "adopt", actual))
                    continue
                if not reviewed and records.get(str(destination)) != actual:
                    raise ValueError(f"Refusing changed or unmanaged target: {destination}\n"
                                     f"Review it, then use --upgrade --replace-reviewed '{destination.name}' to back it up and replace it.")
                plans.append((source, destination, mode, desired, "upgrade", actual))
            else:
                plans.append((source, destination, mode, desired, "install", None))
        except (OSError, ValueError) as error:
            errors.append(str(error))
    if errors:
        raise ValueError("\n".join(errors) + "\nNothing was changed.")
    for source, destination, mode, desired, action, expected in plans:
        if args.dry_run:
            print(f"Would {action}: {destination}" + (" (private backup first)" if action == "upgrade" else ""))
            continue
        no_symlinks(destination)
        if (expected is None and destination.exists()) or (expected is not None and digest(destination) != expected):
            raise ValueError(f"Target changed during installation; rerun after reviewing: {destination}")
        if action != "adopt":
            destination.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
            staging = Path(tempfile.mkdtemp(prefix=".nchat-install-", dir=destination.parent))
            previous = staging / "previous"
            try:
                staged = staging / destination.name
                copy_target(source, staged, mode)
                if digest(staged) != desired:
                    raise ValueError(f"Staged content did not match: {destination}")
                if action == "upgrade":
                    private_directory(state)
                    private_directory(backups)
                    backup_directory = backups / uuid.uuid4().hex
                    private_directory(backup_directory)
                    backup = backup_directory / destination.name
                    copy_target(destination, backup, None if destination.is_dir() else stat.S_IMODE(destination.stat().st_mode))
                    if digest(backup) != digest(destination):
                        raise ValueError(f"Backup verification failed: {destination}")
                    print(f"Backup: {backup}")
                    destination.rename(previous)
                try:
                    staged.rename(destination)
                except BaseException:
                    if previous.exists():
                        previous.rename(destination)
                    raise
            finally:
                shutil.rmtree(staging)
        records[str(destination)] = desired
        save_records(records)
        label = {"install": "Installed", "upgrade": "Upgraded", "adopt": "Adopted reviewed target"}[action]
        print(f"{label}: {destination}")
except (OSError, ValueError, subprocess.CalledProcessError) as error:
    print(error, file=sys.stderr)
    sys.exit(1)

print("\nNo nchat or Kitty configuration was changed, and no app was started.\n"
      "Add the helper bin directory to PATH, then review nchat-example.conf before including it in Kitty.\n"
      "Set attachment_open_command and thumbnail_command in ui.conf if those helpers are not on PATH.\n"
      "Optional dependencies: fzf for nchat-peek; chafa for terminal images (otherwise native Quick Look opens).")
PY
