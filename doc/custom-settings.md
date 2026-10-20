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

Build the helpers with `sh utils/build-menubar.sh`,
`sh utils/build-recorder.sh` and `sh utils/build-transcriber.sh`. Then run
`sh utils/install-macos-helpers.sh --dry-run` to review installation destinations.
Installation preserves existing files and does not change your nchat or Kitty
configuration. Review `utils/kitty-nchat.conf` separately before opting in to its
key/mouse mappings and narrowly restricted badge authorization.

The menu-bar companion accepts `--history-root /path/to/nchat/history` for a
nondefault profile location. Account credentials, databases, cached media and
private drafts must remain outside Git.
