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
presence_poll_interval=120
presence_poll_fast_interval=5
presence_poll_telegram_interval=5
presence_poll_count=50
presence_poll_heartbeat_sec=300
presence_poll_hidden_detect_sec=172800
```

- `presence_poll_enabled` (1/0) turns the poller on or off.
- `presence_poll_interval` is the seconds between polls of the least-recent
  contact (floored at 10; default 120) and `presence_poll_fast_interval` is
  the seconds between polls of the most-recent contact (floored at 5,
  clamped to the slow interval). Every contact in between gets an
  exponentially interpolated interval by rank, so neighbouring ranks differ
  far less at the fast end than at the slow end. Each poll has a random
  phase and +/-20% jitter and requests are dispatched in small bounded
  batches; overdue work is re-phased after pauses. A contact promoted up
  the ranking (for example by a new message) has its next poll brought
  forward into the faster cadence.
- `presence_poll_telegram_interval`: Telegram ignores the gradient and
  polls every contact at this one flat cadence (default 5 s). Each
  Telegram poll is a local TDLib cache read — no per-user server request —
  so the frequent flat rate costs no server load.
- `presence_poll_count` selects the most recent private chats per account
  (50 by default). Their matched Telegram and WhatsApp contacts are also
  included, even outside the other account's recent-chat limit. The final
  tracked set can therefore exceed this count. Setting it to 0 disables
  polling. Groups, self chats, status/newsletter ids and protocols without
  background presence support are excluded.
- Cross-service selection uses complete phone numbers and the existing saved
  matches in `presence-contact-links.json`, with the same matching rules as
  the presence viewer. Edit manual matches with `m` in `nchat-presence`.
  Matching and recent-chat changes refresh within about one second. A person's
  most recent selected conversation determines their polling priority on both
  services. Known private contacts can be sampled even without a conversation
  on that service. Each service retains its own visibility and hidden-status
  handling, and requests keep the existing dispatch limits.
- `presence_poll_heartbeat_sec` re-records the last known status of a contact
  this often (`"hb":1` lines). Heartbeat lines are cached re-affirmations of
  the most recent genuine observation: they prove the poller kept running,
  not that the status was freshly confirmed at that moment.
- `presence_poll_hidden_detect_sec` is the observation window (default 2
  days) after which contacts that never showed any online/last-seen data are
  treated as having their status hidden and are skipped from polling. Setting
  it to 0 disables hidden detection. Only time actually spent in the poll set
  while the account can receive presence counts toward the window: pauses,
  disconnects and (for WhatsApp) announced-offline periods do not count, and
  a long single gap can credit at most five minutes. If such a contact later
  shows data (for example, through a service push), it returns to the poll
  set. To re-detect everyone from scratch, stop nchat and delete the
  contents of `~/.config/nchat/presence/` (deleting them while nchat runs
  does not reset its in-memory bookkeeping).

The history is stored under `~/.config/nchat/presence/` as one JSONL file per
account (`history.<hex-encoded-profile>.jsonl`) plus a small state file.
Writes happen on a private background thread with fsync, partial-append
rollback and crash-tail repair, so slow storage does not stall the
interface; the state file is read once at startup (or on the next poller
enable) and checkpointed on clean shutdown, which waits for pending writes.
If the pending backlog ever overflows its hard cap (a persistently failing
disk), records are dropped — oldest non-marker records first, per profile —
and the loss is reported in the log.
Each sample line is
`{"ts":<ms>,"u":"<userId>","on":0|1,"seen":<ms|-1>,"recent":0|1}` where `seen`
values 1 and 2 mean Telegram's coarse "last month"/"last week" status, 0 is
reserved, and -1 means unknown or hidden; heartbeat re-affirmations add
`"hb":1`. Lines with `"mark":"hidden"` record when a contact was skipped.
Only genuine service observations are recorded, never guesses derived from
the UI display state. Telegram sampling reads TDLib's local user cache (an
offline read — no per-user server request); the server-side freshness comes
from the single `contacts.getStatuses` request TDLib already issues on
login/online-privacy/contact-sync events plus status pushes while online.
WhatsApp samples come from presence subscriptions, so WhatsApp data only
flows while this installation is online (see `online_status_share`). The
files stay on this machine and are never uploaded.

### Latest reported status screen

Run `nchat-presence` or `nchat-presence current` for one row per person, with
separate Telegram and WhatsApp columns. Matching complete phone numbers join
automatically, as do the same service identity across accounts. Each column
keeps its own status and timestamps. A dash means that service has no contact in
the current view. The selected-person panel shows both service identities and
their report details. The header shows when the local cache was read.
Reports older than five minutes are labelled old or unconfirmed. Cached
heartbeats do not renew freshness. Flags render as country letters, with emoji
modifiers and joiners simplified for stable terminal columns.

Use arrows or j/k to select, Tab to cycle All/WhatsApp/Telegram, and /, Ctrl+F
or f to search names, aliases or numbers. Formatted numbers with spaces, plus
signs, parentheses and hyphens work. Results filter as you type. Enter finishes
the query and arrow/page keys immediately navigate its results. Esc clears, r reads the
cache again and q to quit. Page Up/Page Down and g/G move through longer lists.
Tab filters people who have the selected service while preserving both columns.
Press **Enter** on a selected contact to open their online-period table: the
times they came online and went offline, merged across services, newest
first, with an estimated length (`~15m`) per session. `?` marks a session
whose end the platform never recorded, `open?` a session with no closing
observation yet, and `~` flags that the start may predate the first
observation. q or Esc returns to the list. The legacy `show` command still
prints the raw samples for full detail.
The list itself uses columns from 45 terminal columns and compact person
cards below that width. `--plain` prints the paired-column snapshot and is
selected automatically for piped output. `NO_COLOR=1` requests monochrome.
The screen updates displayed ages between refreshes. Press r for newer
records.

When phone metadata is missing, select a person and press **m**. Choose their
contact on the other service by typing their name or number immediately. Use
arrows to choose and press Enter once to link, or Escape to cancel. The picker
includes cached private contacts that have no presence observations, excludes
self/group identities, and places matching names first. A linked counterpart
without an observation shows **No report**. Name equality alone leaves identities separate until this
confirmation. Search matches all linked aliases, identifiers and full cached numbers. Press **u** to
remove a selected person's manual links. Automatic phone/account matches still
apply. Multiple accounts retain their records, with the newest genuine report
chosen for each service column.

Confirmed display links are stored privately in
`<config>/presence-contact-links.json` with mode `0600`, using a lock and atomic
replacement. These are local display preferences. Link and Unlink update that
file. Browsing and refresh read the existing cache and reports. Invalid saved
preferences preserve the automatic view and display a warning, and a failed save
retains the previous file.

The weighted review and its checks are in
[nchat presence UI scorecard](nchat-presence-ui-review.md).

Read the history with the explicit legacy `nchat-presence` commands:
`nchat-presence list [pattern]` shows every tracked contact with its latest
state; `nchat-presence summary [pattern]` summarizes one contact (first
sample, last record, last state, genuine online events, heartbeat count,
hidden mark); and `nchat-presence show <pattern>` prints the most recent
history rows (the last 200 by default; `--all` removes the cap; hidden
markers share the budget). Patterns match user ids/numbers and, when the
local cache is readable, display names (case-insensitive); `--no-names`
skips the cache lookup, `--profile` filters by account,
`--dir`/`--appdir` override the locations, `--since`/`--until` take
`YYYY-MM-DD [HH:MM]` (applied to markers too), and
`--no-heartbeats`/`--online-only` filter rows. Timestamps carry a numeric
UTC offset. Heartbeat rows re-affirm the last genuine observation and are
never counted as new online events; a contact is shown as skipped only
while its hidden mark is not followed by any observable sample, and the
samples are historical records — they cannot establish that someone is
online right now. WhatsApp contacts are matched by their full JID
(`<number>@s.whatsapp.net`) or a substring of it; display names resolve
through the cache's phone column only for complete, unmasked numbers
(masked entries stay ids). The helper only reads the local files; it never
contacts any service.

### Complete emoji in Kitty and GNU Screen

Install `nchat_emoji.py` and `nchat_terminal_auth.py` alongside the existing
`nchat_badge.py` and `nchat_badge_auth.py` helpers in Kitty's configuration
folder. The macOS helper installer includes all four. In `kitty.conf`, use
`allow_remote_control password` and
`remote_control_password "" nchat_terminal_auth.py`, then reload Kitty's
configuration before restarting nchat. The helper accepts bounded glyph
painting in the originating window and the existing badge operation.

When Kitty's window environment and the rendering helper are present, nchat
reserves display columns using complete emoji sequences and paints the original
Unicode in those cells. Joined emoji, flags, keycaps, skin tones and combining
accents keep their source text and wrapping boundaries. Screen receives ASCII
metadata and simple cell placeholders, avoiding its joined-emoji width errors.
A bounded repaint restores glyphs after a Screen redisplay or reattachment.

This path is tested with Kitty 0.48.2 and GNU Screen 5.0.2 on macOS. Other
terminals retain the existing compatibility rendering. `NCHAT_FULL_EMOJI=0`
disables Kitty glyph painting for troubleshooting. `NCHAT_KITTY_CONFIG_DIR`
selects an alternative helper folder. This covers emoji sequences and combining
accents, rather than being a general complex-script shaping engine.
