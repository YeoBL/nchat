# Optional personal-fork settings

These are portable examples, not an account export. Merge only the settings you
want into your own configuration; do not replace a whole existing file.

In `app.conf`:

```ini
message_delete=3
```

This retains messages already received by this installation and marks them as
deleted. It applies to WhatsApp and, in this fork, Telegram. It cannot restore
content deleted before nchat received it. It also retains Secret Chat deletions
in nchat's ordinary local cache; do not assume self-destructing content is erased.

In `ui.conf`, after installing the optional helpers and adding their bin
directory to `PATH`:

```ini
attachment_open_command=nchat-image '%1'
thumbnail_command=nchat-thumbnails
muted_notify_unread=1
terminal_bell_inactive=0
```

The last two lines reproduce this fork's optional terminal-notification
preferences; the menu-bar companion still excludes muted chats. Native recording
and on-device transcription use helper apps in the configuration directory's
`helpers/` folder, or the `record_app` / `transcribe_app` overrides in `ui.conf`.
These overrides name complete `.app` directories, not executable commands.
The old `transcribe_command` key is ignored by the current transcription path.
Alt-Shift-t starts a job without waiting in a tools dialog; close tools to keep
chatting, then use **F2 → Transcription status** for progress, full results or
errors. Completed text also appears in **Search saved audio transcripts**.

Build the helpers with `sh utils/build-menubar.sh`,
`sh utils/build-recorder.sh` and `sh utils/build-transcriber.sh`. Then run
`sh utils/install-macos-helpers.sh --dry-run` to review installation destinations.
Installation preserves existing files and does not change your nchat or Kitty
configuration. This means a normal install does **not** upgrade older helpers.
To review an upgrade, use `sh utils/install-macos-helpers.sh --upgrade --dry-run`.
Only unchanged files managed by this installer can be upgraded automatically;
each replacement is backed up privately. Older unmanaged files or local edits
are kept until you inspect their differences and explicitly name that target,
for example `--upgrade --replace-reviewed nchat-peek`. Repeat
`--replace-reviewed NAME` for each separately reviewed helper or app bundle.
Never use this option to discard custom helper code without reviewing it.
Use repeatable `--only NAME` to limit installation or upgrades to those targets;
for example `--upgrade --only nchat-peek --replace-reviewed nchat-peek`.
Verified backups are stored under
`$NCHAT_HELPER_PREFIX/.nchat-helper-install/backups` (normally
`~/.local/.nchat-helper-install/backups`); keep the printed backup path for
manual recovery. Python 3 is required by the installer and cached-chat browser.

The installer reuses an existing `/Applications/nchat Unread.app` by default;
otherwise the menu-bar app goes to `~/Applications`. Set
`NCHAT_APPLICATIONS_DIR` to choose explicitly. `NCHAT_HELPER_PREFIX`,
`NCHAT_CONFIG_DIR` and `NCHAT_KITTY_CONFIG_DIR` override the other destinations.
Installation does not start or restart apps, grant media permissions, or alter
Kitty mappings. Review `utils/kitty-nchat.conf` separately before opting in to its
key/mouse mappings and narrowly restricted badge authorization.

The menu-bar companion accepts `--history-root /path/to/nchat/history` for a
nondefault profile location. Account credentials, databases, cached media and
private drafts must remain outside Git.
