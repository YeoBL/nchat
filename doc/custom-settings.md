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

With `nchat-image`, Ctrl-V opens video notes and other detected videos in
QuickTime Player, avoiding the macOS Quick Look video-player crash. Images keep
the terminal viewer and its `P` shortcut for Preview; audio/documents and the
missing-terminal-renderer fallback keep their existing Quick Look handling.
Updating this standalone helper takes effect on the next open without restarting
nchat. Playback still depends on the file being complete and its codec supported.

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

## Continuous presence polling

While nchat runs, a background poller samples the online/last-seen status of
the most recent private contacts of each account and keeps a local activity
history. In `ui.conf`:

```ini
presence_poll_enabled=1
presence_poll_interval=60
presence_poll_count=30
presence_poll_heartbeat_sec=300
presence_poll_hidden_detect_sec=172800
```

- `presence_poll_enabled` (1/0) turns the poller on or off.
- `presence_poll_interval` is the nominal seconds between polls of each
  contact (floored at 10). Every contact starts at a random offset inside
  the interval and drifts by +/-20% per reschedule, so requests never arrive
  in one burst. After a long pause the requests are re-phased instead of
  firing at once.
- `presence_poll_count` is the number of most recent private chats polled per
  account (30 by default). Groups, self chats, status/newsletter ids and
  protocols without background presence support are excluded.
- `presence_poll_heartbeat_sec` re-records the last known status of a contact
  this often (`"hb":1` lines). Heartbeat lines are cached re-affirmations of
  the most recent genuine observation: they prove the poller kept running,
  not that the status was freshly confirmed at that moment.
- `presence_poll_hidden_detect_sec` is the observation window (default 2
  days) after which contacts that never showed any online/last-seen data are
  treated as having their status hidden and are skipped from polling. Setting
  it to 0 disables hidden detection. Only time actually spent in the poll set
  while the account can receive presence counts toward the window (pauses,
  disconnects and announced-offline periods do not); if such a contact later
  shows data (for example, through a service push), it returns to the poll
  set. To re-detect everyone from scratch, stop nchat and delete the
  contents of `~/.config/nchat/presence/` (deleting them while nchat runs
  does not reset its in-memory bookkeeping).

The history is stored under `~/.config/nchat/presence/` as one JSONL file per
account (`history.<hex-encoded-profile>.jsonl`) plus a small state file.
Writes happen on a private background thread with fsync, partial-append
rollback and crash-tail repair, so slow storage does not stall the
interface; the state file is read once at startup (or on the next poller
enable) and again on clean shutdown, which waits for pending writes.
Each sample line is
`{"ts":<ms>,"u":"<userId>","on":0|1,"seen":<ms|-1>,"recent":0|1}` where `seen`
values 1 and 2 mean Telegram's coarse "last month"/"last week" status, 0 is
reserved, and -1 means unknown or hidden; heartbeat re-affirmations add
`"hb":1`. Lines with `"mark":"hidden"` record when a contact was skipped.
Only genuine service observations are recorded, never guesses derived from
the UI display state. Telegram samples use the status TDLib already keeps
for recent chats; WhatsApp samples come from presence subscriptions, so
WhatsApp data only flows while this installation is online (see
`online_status_share`). The files stay on this machine and are never
uploaded.
