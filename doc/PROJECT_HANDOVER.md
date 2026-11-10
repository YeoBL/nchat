# nchat project handover for the next assistant

This is Bing Lin's personal nchat fork and its macOS menu-bar companion. Use this
handover to locate the live installation, implement changes through the normal
UI, test them, deploy them into the existing session, and preserve their history
on GitHub. The operational snapshot was checked on **9 November 2026**, in
Europe/London. Process IDs, terminal names and connection state require a fresh
check before an action.

## Start here

1. Work in `/Users/bingl/.local/src/nchat-inline`. The Codex chat's default
   directory `/Users/bingl/Documents/ChatGPT/chatbot` is a different directory.
2. Read the repository's `AGENTS.md`, this handover and the relevant rows in
   `changes.md`. Local `AGENTS.md` and the user's current request govern the work.
3. Inspect `git status`, the current process tree and Screen windows. Preserve
   unrelated changes. Locate the exact binary and account directory in use.
4. Trace the UI entry point, shared request type, service implementation and
   cache effects before editing. A cache-only change cannot implement a cloud
   action such as sending, deleting or marking messages read.
5. Test the relevant production paths with synthetic accounts. Build, activate,
   verify, update the feature inventory, commit focused changes and push to the
   personal fork.

## Live installation snapshot

| Item | Observed value |
|---|---|
| Source checkout | `/Users/bingl/.local/src/nchat-inline` |
| Working branch and GitHub default branch | `codex/personal-features` |
| Source before this handover | `9e4bc1a39531630b97b7c59f09e2c12c67e24b19` |
| Working tree before this handover | Clean |
| Personal remote | `origin`, `https://github.com/YeoBL/nchat.git` |
| Original upstream | `upstream`, `https://github.com/d99kris/nchat.git` |
| Comparison baseline | Annotated `vanilla-base` tag, nchat 5.17.26, upstream `ea41fd4` |
| Latest saved source checkpoint | `checkpoint/nchat-2026-11-09`, targeting `9e4bc1a3` |
| Earlier checkpoint | `checkpoint/nchat-2026-11-09-before-backup`, targeting `da56b058` |
| Core executable | `/Users/bingl/.local/src/nchat-inline/build/bin/nchat` |
| Shell command wrapper | `/Users/bingl/.local/bin/nchat`, which execs that binary |
| Screen server | PID `15976`, session `15976.nchat`, attached |
| Required restart destination | The same Screen session, window `0`, title `zsh` |
| Original shell in that window | PID `15977`, `/bin/zsh`, terminal `ttys002` |
| Current core process | PID `86901`, parent `15977`, terminal `ttys002` |
| Screen dimensions | 216 columns by 64 rows |
| Menu-bar app | `/Applications/nchat Unread.app` |
| Widget executable | `/Applications/nchat Unread.app/Contents/MacOS/nchat-menubar` |
| Current widget process | PID `78846` |
| User config and account root | `/Users/bingl/.config/nchat` |
| Login agent | `/Users/bingl/Library/LaunchAgents/local.nchat.unread.plist` |
| Kitty config | `/Users/bingl/.config/kitty/kitty.conf` |
| Screen config | `/Users/bingl/.screenrc` |

The core binary and protocol libraries had modification times on 8 November.
The installed widget binary had a modification time on 5 November. The source
checkpoint also contains later documentation and standalone experimental-probe
work. A source commit, executable timestamp and successful connection each
establish different facts about what is running.

Recheck the live processes with scoped commands.

```sh
cd /Users/bingl/.local/src/nchat-inline
git status --short
git log -5 --oneline
ps -axo pid,ppid,%cpu,stat,tty,etime,comm | rg 'nchat|SCREEN'
screen -ls
screen -S 15976.nchat -Q windows
screen -S 15976.nchat -p 0 -Q info
```

Bound Screen queries with a short timeout in automated diagnostics. A stuck
Screen server can also leave `screen -Q` clients waiting.

## User preferences and standing authorization

- The user authorizes restarting both core nchat and the widget whenever needed.
  This was expanded to core restarts on 5 November. Save unsent drafts where
  practical. Widget reply drafts survive dismissal but are lost on app restart.
- Reuse the same Screen session and window for every core restart. Confirm that
  window remains selected. If reuse is unavailable, explain that before choosing
  an alternative. Earlier creation of a different window confused the user and
  caused apparent lock failures when they tried to start another copy.
- Work through the requested behavior to a usable result. Ask for missing
  information only when the existing context and connected sources cannot settle
  it. Give concise progress updates during sustained work.
- Sending messages, voting, deleting, changing memberships and other real service
  mutations need authorization for that action. Restart permission covers
  restarts. Use fake providers for send and vote tests and preserve ambiguous
  send outcomes for the user to resolve.
- Create a separate `codex/` feature branch and isolated worktree for each task.
  Other agents may work concurrently. Integrate the latest commits from
  `codex/personal-features`, resolve conflicts and rerun checks before merging
  back there. This is the fork's integration/default branch, called main by the
  user. Push the completed merge and clean up only your own worktree. Never
  discard another agent's uncommitted changes or remove their worktree.
- Commit each completed feature or later fix separately. Include helper scripts,
  widget changes, tests and documentation. Update the root `changes.md` table
  with description, implementation commit, access, checks and limitations.
- Push only to `YeoBL/nchat`. Preserve original history and user changes. Some
  early feature commits reconstruct an older uncommitted implementation, and
  shared prerequisites mean selective rollback can require a follow-up patch.
- Keep account databases, real messages, names, phone numbers, drafts, recordings,
  media, credentials, QR codes and session secrets outside Git and public logs.
- Search within the repository or a specifically relevant config path. A past
  broad filesystem search triggered many macOS folder, music, photos and other
  app-data prompts attributed to Kitty. Scope `rg` explicitly.
- Remove assistant-created temporary files after use, including cancelled-task
  downloads, duplicate exports and intermediate artifacts. Use Trash where
  practical. Preserve final deliverables, pre-existing files and deliberately
  retained recovery records. Close assistant-opened browser tabs after use.
- Prefer small shared fixes and existing APIs. Delegate to subagents when the
  current user request or applicable instructions calls for them. Old agent
  names in conversation context are historical metadata.

## Architecture and source map

The C++ terminal app owns the UI, local feature state and message-cache access.
Protocol workers exchange `RequestMessage` and `ServiceMessage` objects through
the shared protocol interface. Telegram uses TDLib. WhatsApp uses a C++ bridge
into Go and the vendored whatsmeow library. The separate Swift/AppKit widget
reads cached history and sends explicit actions through narrow filesystem IPC.
`src/ui.cpp` owns the widget-action and download bridge lifetimes.

| Change area | Start editing here | Follow through to |
|---|---|---|
| Main behavior and tools routing | `src/uimodel.cpp` | Protocol requests, cache callbacks, dialog/view state |
| Shared account requests and result types | `lib/common/src/protocol.h` | Both service implementations and every caller |
| Telegram client and events | `lib/tgchat/src/tgchat.cpp` | TDLib callbacks, mute/membership/forum handling |
| Telegram tools | `lib/tgchat/src/tgfeatures.inc`, `tgfeatures_manage.inc`, `tgfeatures_social.inc`, `tgfeatures_util.h` | Actual Telegram requests, pagination, permission/error handling |
| WhatsApp C++ bridge | `lib/wmchat/src/wmchat.cpp` | `lib/wmchat/go/cgowm.go`, `gowm.go`, native callback types |
| WhatsApp account actions | `lib/wmchat/go/actions*.go` | The relevant messages, contacts, channels, history or status file |
| WhatsApp receipts and historical recovery | `lib/wmchat/go/receipts.go`, `actions_history.go` | `src/uimessageinfodialog.h`, cache ledger, exact history anchors |
| WhatsApp poll updates and persistence | `lib/wmchat/go/poll_store.go`, `gowm.go` | Poll requests, encrypted updates, aliases and withdrawals |
| Persistent history and metadata | `lib/ncutil/src/messagecache.cpp`, `messagecache.h` | Schema migration, stale replay, mutex boundaries, callbacks |
| Pending unread accounting | `src/notificationstate.h`, `src/uimodel.cpp` | Cache snapshots, service read markers, membership/mute/archive |
| Poll dialog | `src/uipolldialog.h` | `GetPollVotesRequest`, `VotePollRequest`, service limits |
| Receipt and reaction dialogs | `src/uimessageinfodialog.h`, `src/uireactioninfodialog.h` | Supplied identities/timestamps and cancellation |
| Tools list and forms | `src/uiclientactiondialog.h`, `src/uitextinputdialog.cpp` | F2/F3 routing, action review, defaults, focus and retry |
| Local drafts, folders and bookmarks | `src/localfeatures.h` | Private atomic state file and per-chat/account identity |
| History, list and status drawing | `src/uihistoryview.cpp`, `src/uilistview.cpp`, `src/uistatusview.cpp` | `src/uicursestext.h`, `lib/ncutil/src/strutil.cpp` |
| Composer and terminal keys | `src/uientryview.cpp`, `src/uikeyinput.cpp`, `src/uikeyconfig.cpp`, `src/uicontroller.cpp` | User key config, curses widths, Kitty and Screen translation |
| Widget UI, cache query and animation | `utils/nchat-menubar.swift` | Text selection, link attributes, poll panel, native event handling |
| Swift action transport | `utils/nchat-widget-reply.swift` | Core `src/widgetreplies.h` and service acknowledgements |
| Core widget actions | `src/widgetreplies.h` | Exact account/chat/message validation and provider callbacks |
| Explicit cached attachment downloads | `src/peekdownloads.h` | Swift/Python requester and service file callbacks |
| Image/video opener and thumbnails | `utils/nchat-image`, `nchat-thumbnails`, `nchat-image-clear` | Installed copies under `~/.local/bin` |
| Cached all-chat browser | `utils/nchat-peek`, `utils/nchat-peek-view.py` | Installed Python file under `~/.local/lib` |
| Recorder and transcriber | `utils/nchat-record.swift`, `utils/nchat-transcribe.swift` | `src/featuremedia.h`, `src/featureutil.h`, native helper apps |
| Kitty badge integration | `src/terminalbadge.h`, `utils/nchat_badge.py`, `utils/nchat_badge_auth.py` | Narrow opt-in Kitty authorization and acknowledged badge updates |
| Screen navigation workaround | `utils/screen-scroll.conf` | Its source line in `~/.screenrc` and loaded Screen settings |
| Isolated login experiment | `lib/wmchat/go/cmd/viewonce-probe/` | `doc/viewonce-probe.md`, fresh private bounded test profile |

Resolve abbreviated filenames against the directory named earlier in their row.
The whatsmeow source is vendored under `lib/wmchat/go/ext/whatsmeow`. Its retained
patch is `lib/wmchat/go/ext/nchat-whatsmeow.patch`. Review the existing
`utils/whatsmeow-update` and `utils/whatsmeow-mkpatch` workflow before changing
library versions. Telegram's generated TDLib objects and library build are under
`lib/tgchat/ext/td` and `build`.

## Runtime files and configuration

| Path under `/Users/bingl/.config/nchat` | Purpose |
|---|---|
| `app.conf` | Application options, including `message_delete=3` |
| `ui.conf` | UI, status, preview helper and notification settings |
| `key.conf`, `color.conf` | Actual keyboard mappings and colours |
| `profiles/` | Account/session state with credentials |
| `history/<profile>/db.sqlite` | Per-account message and metadata cache |
| `local-features/state` | Persistent terminal drafts, local folders and bookmarks |
| `widget-replies/` | Private reply/read/poll requests and acknowledgements |
| `peek-downloads/` | Private explicit-download requests |
| `widget-dismissed.json` | Exact message IDs hidden by local Clear |
| `menubar-state.json` | Widget diagnostic snapshot |
| `menubar-error.log`, `log.txt` | Widget launch errors and core logs, potentially private |
| `helpers/` | Recorder/transcriber helper apps |
| `temp/` | Private runtime temporary files |

The directory-level account lock is implemented by `ScopedDirLock` using
`flock` on the directory descriptor. A surviving process can keep it held after
its Screen client disappears. Identify that process and its open descriptors
before recovery. Removing a supposed stale lock file cannot release this lock.
The bridge has an additional private lock and server marker.

Currently selected local settings include the following.

```ini
# app.conf
message_delete=3

# ui.conf
attachment_open_command=/Users/bingl/.local/bin/nchat-image '%1'
thumbnail_command=nchat-thumbnails
mark_read_on_view=1
mark_read_when_inactive=0
mark_read_any_chat=0
muted_notify_unread=1
online_status_dynamic=1
online_status_share=1
status_enabled=1
status_broadcast=1
```

The widget still filters muted chats even though this user's terminal allows
its configured muted notification policy. Opening a chat through ordinary
terminal navigation can mark messages read under these settings. Cached preview
and explicit read actions have separate behavior.

`message_delete=3` preserves content previously received and adds deletion state
for both services. Older content that never reached this installation cannot be
recovered from a placeholder. The retention setting also applies to Secret Chat
deletions in the ordinary local cache, so it affects the expected expiry model.

### Widget action boundaries

The `widget-replies/server` marker contains a process ID, random nonce and
capability strings including `read-v1` and `poll-v1`. Request files are private,
nonce-bound, age-limited and targeted to exact account/chat/message identities.
Requests use `.request`, `.read`, `.poll` or `.vote` and receive `.result` files.
Keep the ownership, mode, symlink, length, deduplication and stale-server checks
when extending the bridge. Marker contents belong in private runtime state.

Reply acceptance, delivery and read confirmation are distinct states. The
send-and-read shortcut captures the visible unread IDs at submission, sends
once, and requests read only after acceptance. Messages arriving after submission
remain eligible for notification. Ambiguous timeouts retain uncertainty and
must be resolved without automatically resending the reply.

Clear stores local dismissal IDs. Mark as read must reach the service and
reconcile its result. Telegram's fixed path loads exact targets, uses the
appropriate chat/forum source, and confirms the resulting read checkpoint.
Metadata-only poll queries fetch counts and eligibility with `includeVoters=false`.
Voting and withdrawal require the user's explicit action.

## Build and installation

This Mac uses Apple Silicon, Homebrew under `/opt/homebrew`, ncurses 6.6,
Screen 5.0.2, Go 1.26.5 and Apple Swift 6.4 at the handover snapshot. Existing
CMake configuration enables Telegram, WhatsApp, dynamic protocol loading and
shared libraries. Signal is disabled. Use the existing configured build first.

```sh
cd /Users/bingl/.local/src/nchat-inline
cmake --build build -j 4
```

For service work, build the relevant library as well as the core.

```sh
cmake --build build --target wmchat nchat -j 4
cmake --build build --target tgchat nchat -j 4
```

The live core loads libraries from `build/lib`, including `libncutil.dylib`,
`libwmchat.dylib`, `libtgchat.dylib` and TDLib support. A successful core-only
link does not establish that an edited protocol library has rebuilt. The Go
c-archive build deliberately builds the bridge package itself, excluding
standalone commands under `cmd/`. Earlier building all main packages broke it.

The widget build compiles the two Swift sources, constructs an app bundle,
ad-hoc signs it and runs synthetic/native layout checks.

```sh
sh utils/build-menubar.sh
sh utils/build-recorder.sh
sh utils/build-transcriber.sh
```

Run only the helper builds relevant to the change. Inspect installation before
applying an upgrade.

```sh
sh utils/install-macos-helpers.sh --upgrade --only 'nchat Unread.app' --dry-run
sh utils/install-macos-helpers.sh --upgrade --only 'nchat Unread.app'
```

The installer preserves existing files and upgrades unchanged installer-managed
targets. For legacy or edited targets, inspect the exact diff first, then use
`--replace-reviewed NAME` for that target if replacement is intended. It retains
private backups under `~/.local/.nchat-helper-install/backups`. Installing a
helper takes effect through the installed path, so editing only its repo copy
can leave the live behavior old. The installer leaves starting/restarting apps
to the caller. Its environment overrides and detailed rules are in
`doc/custom-settings.md`.

The existing app is in the main `/Applications` directory and the login agent
opens it with `/usr/bin/open -g -W`. The agent runs at login and has a failure
restart policy. Starting the widget directly uses the following command.

```sh
open -g '/Applications/nchat Unread.app'
```

For a widget replacement, preserve its draft, stop the identified widget process
cleanly, install the tested bundle and relaunch it. Check the resulting process
path and signature/binary identity. A widget-only change ordinarily requires
only a widget restart. Protocol, cache or core IPC changes require a core restart
and sometimes a matched widget upgrade.

## Restarting core nchat in the existing window

1. Rediscover the current core PID, parent shell, terminal, Screen session and
   window. Compare these with the snapshot above. Preserve any draft.
2. Check that the identified process really is the expected core executable.
   Send its configured clean quit key through that exact Screen window.
3. Wait for the original process to exit and its existing shell to regain the
   foreground terminal process group. Shutdown can wait for queued cache writes.
4. Start the tested executable in that same shell. Select the original window
   and verify the new PID, executable, parent and terminal.

For the snapshot session, the quit command is the following. Recheck its identity
before using it.

```sh
screen -S 15976.nchat -p 0 -X stuff $'\021'
```

The existing shell's `pgid` and `tpgid` should match before sending a shell command.

```sh
ps -p 15977 -o pid,pgid,tpgid,tty,comm
```

After clean shutdown, launch into the original window.

```sh
screen -S 15976.nchat -p 0 -X stuff $'NCHAT_NO_AUTO_SELECT=1 /Users/bingl/.local/src/nchat-inline/build/bin/nchat\r'
screen -S 15976.nchat -X select 0
```

`NCHAT_NO_AUTO_SELECT=1` postpones automatic chat selection/read behavior until
interaction. It is useful for a restart or passive synchronization check.
If the session is detached, the user can reattach with `screen -r nchat`.
Terminal writes sent to a window still owned by nchat would become application
input, which is why the foreground-shell check matters.

If clean shutdown stalls, diagnose the current stacks and cache activity before
escalating. A force stop has draft/cache consequences. Broad process-name kills
can affect the widget or an unrelated test process. Once restarted, verify both
connection state and actual UI responsiveness. Count-only, redacted diagnostics
are preferable to copying full private logs into a report.

## Test selection

These are existing entry points. Choose tests tied to the changed paths and
read each script before running it. Several native tests link existing build
objects, making a fresh relevant build important.

| Area | Existing checks |
|---|---|
| UI tools, search defaults, forms and focus | `sh utils/test-tools-dialog-ux.sh` |
| F2–F6 integration, drafts and edit behavior | `sh utils/test-ui-feature-integration.sh` |
| Cache, notifications, membership, receipts, read ordering and Unicode drawing | `bash utils/test-notifications.sh` |
| Widget action IPC | `bash utils/test-widget-replies.sh` |
| Widget UI and native layout | `sh utils/build-menubar.sh`, or built widget `--self-test` and `--test-popover-layout` |
| Poll dialog and generated Telegram metadata | `sh utils/test-polls.sh build` |
| Telegram mute inheritance and action callbacks | `sh utils/test-telegram-mutes.sh` |
| Telegram poll voter pagination | `sh utils/test-telegram-poll-pages.sh` |
| Telegram per-message edit permissions | `sh utils/test-telegram-edit-permissions.sh` |
| Telegram explicit read action | `sh utils/test-telegram-mark-read.sh` |
| Telegram view-once labeling and download suppression | `sh utils/test-telegram-view-once.sh` |
| WhatsApp cache-backed contact recovery | `bash utils/test-whatsapp-contact-sync.sh`, which accesses the real local cache read-only |
| WhatsApp live join and rename handlers | `sh utils/test-whatsapp-group-names.sh` |
| WhatsApp Go implementation | In `lib/wmchat/go`, `go test -race ./...` and relevant focused tests |
| Isolated experimental probe | In `lib/wmchat/go`, `go test -race -count=1 ./cmd/viewonce-probe` and `go vet ./cmd/viewonce-probe` |
| Screen queue freeze | `python3 utils/test-screen-scroll.py` |
| Helper upgrade safety | `python3 utils/test-macos-helper-install.py` |
| Cached-browser synthetic behavior | `python3 utils/nchat-peek-view.py --self-test` |
| Presence poller scheduling and persistence | `sh utils/test-presence-poller.sh` |

Go tests involving the C++ host logger can need the existing wrapper's native
stubs. Follow that wrapper when linker symbols are unresolved. A bare test
failure from missing host symbols is different from a service behavior failure.
Synthetic inputs and fake protocols provide reproducible checks without real
messages, votes or account changes. Relevant service/UI round trips still need
their own truthful verification status.

### Regression testing traps

- Kitty's embedded Python runs optimized and disables Python `assert` statements.
  An earlier terminal-grid test silently skipped its checks. The corrected
  `utils/test-history-terminal.py` raises explicitly and uses a deliberately
  wrong-cell negative control. It checks every incremental frame rather than
  only the last repaint.
- The corrected rendering fixture covers 20 frames in each of standalone xterm,
  Screen at 96 by 24 and Screen at 216 by 64. It verifies rows and ASCII cell
  positions through independent terminal parsing and fragmented Screen relay.
- Production builds can define `NDEBUG`. Ensure assertion-based test translation
  units actually retain their assertions and negative cases fail as expected.
- An AppKit hidden-window or synthetic cache test establishes that exact scope.
  Real clipboard, browser, Preview, service pagination and message delivery
  require separate checks. State this beside the affected feature.
- Some test scripts leave generated binaries/objects in `build`. Remove the
  assistant-created test outputs after use while preserving app build products.

## Incidents and constraints to preserve

### Presence history layout and incorrect manual matches

On 10 November 2026, a contact appeared to disappear because saved manual links
merged distinct Telegram identities into one WhatsApp person. The viewer chose
one of their names for the combined row. The reported associations were repaired
privately with a recovery copy under
`~/.local/state/nchat/presence-link-backups/`. Private identities and matching
preferences are excluded from Git. The core reads preference changes on its
next selection refresh.

`e172760c` excludes already-paired people from the other-service picker and
rejects a second conflicting same-account identity at the preference writer.
`c183761f` replaces the history's `+N` day offsets with full dates and seconds,
full service names, explicit unknown ends, compact narrow layouts and a range
counter. Reload retains history position. `utils/test-presence-current.py`
checks both the matching failure and the layouts, including native PTY history
entry, resize and exit. The installed Python helper must be reopened to load
new code. No core restart is required for these helper changes.

### Cross-service presence selection

Implementation `68bc2050` changes `UiModel::Impl::ProcessPresencePoll` to use a
shared set built by `SelectPresenceContacts` and `src/presencecontacts.h`.
Each account contributes its most recent `presence_poll_count` private chats.
Complete phone matches and the existing private `presence-contact-links.json`
manual links connect their counterparts across Telegram and WhatsApp. Every
known private identity in a selected group is tracked, including accounts with
no conversation for that identity. The final set can exceed the initial count.
The graph refreshes at most once a second. Existing per-service scheduling,
availability and hidden-status behavior still apply. Manual-link validation
falls back to automatic matching on invalid/unreadable snapshots and does not
write the user's matching preferences. `utils/test-notification-handlers.cpp`
contains the production-model regression, including counterpart persistence.
The full build, production notification/rendering regressions, standalone
scheduling/persistence tests and presence viewer self-test passed. Restarted on
10 November 2026 in original Screen `15976.nchat`, window `0`, shell `15977`,
terminal `ttys002`, core PID `54177`. The frame returned Online. The previously
unrecorded WhatsApp counterpart entered the tracked set, delivered a real
last-seen update, and appeared in the installed viewer alongside Telegram.
No test message or explicit read receipt was sent. See `changes.md`.


### Complete emoji rendering in Kitty

The `codex/full-emoji-rendering` feature replaces split-component rendering in
Kitty with intact Unicode glyphs. Implementation commit `f1f714dd` adds
`lib/ncutil/src/textglyph.h`, `src/uicursestext.cpp`, `utils/nchat_emoji.py`
and `utils/nchat_terminal_auth.py`. The layout iterator is shared by width,
clipping, wrapping and cursor positioning. All terminal view refreshes use
`UiCursesText` so cell overlays are invalidated when windows redraw or overlap.

GNU Screen corrupts non-ASCII DCS payloads and miscounts joined emoji and some
combining accents. The core reserves protected ASCII/ideographic-space cells,
then sends ASCII-escaped JSON in bounded Screen passthrough chunks. A fixed Kitty
kitten paints whole glyphs in the originating window and restores cursor and
style state. Calls are batched, bounded and refreshed after Screen redisplays.
The authorization helper preserves existing badge permissions and additionally
allows only validated glyph painting. The installer includes the new helpers.

Load `nchat_terminal_auth.py` through the `remote_control_password` setting and
reload Kitty configuration before starting the patched core. File presence and
Kitty's window environment activate this path. `NCHAT_FULL_EMOJI=0` disables it.
The legacy fallback below still applies in other terminals. This is tested with
Kitty 0.48.2 and Screen 5.0.2, and is not general complex-script shaping.
Screen hardcopy and Screen copy mode retain placeholders. Kitty's actual text
grid and selection contain the rendered Unicode. The native grid/Screen replay
tests use synthetic data and never send service messages or read requests.

The complete build and the notification/rendering, helper-installation,
tools-dialog UX and UI integration suites passed. Dialog/integration tests also
passed with full rendering explicitly enabled. Dense replay includes 180 joined
emoji and multiple bounded paint calls. On 9 November the core was restarted in
Screen `15976.nchat`, window `0`, shell `15977`, terminal `ttys002`, PID `44174`.
It returned Online. The Screen session was initially detached. Kitty helpers and
the narrow authorization setting are installed with a private configuration
backup. After reattachment and the Kitty configuration reload, the user
confirmed “Complete emoji and aligned text”. This confirms acceptance of the
helper and the actual live appearance, in addition to the native parser and
isolated Screen replay checks.

### Joined emoji rendering follow-up

On 9 November 2026, two adjacent U+1F642 U+200D U+2194 U+FE0F
head-shaking emoji exposed another cursor-width mismatch. Curses allocates
columns for the component symbols while Kitty combines the sequence. Incremental
redraws can then shift text and leave old divider/header fragments behind.

`src/uicursestext.h` applies a display fallback shared by history, sidebar,
status, dialogs and composer. It omits joiners, presentation selectors, keycap
decoration and emoji tag characters during drawing. Component symbols use their
default widths. Existing skin-tone behavior still applies. Message storage,
clipboard and outgoing text use the original source. Combined-emoji appearance
is simplified in the terminal. Do not remove this fallback without an equivalent
end-to-end curses/Screen/Kitty width test.

`sh utils/test-notifications.sh` includes the expanded rendering fixtures with
head shakes, family/profession combinations, presentation selectors, keycaps and
emoji tags. The regression failed on the original drawing path. The repaired
path passed every frame in native Kitty and an isolated Screen relay, including
scrolling and independently dirty panels. No private message fixture is stored.

The full build, notifications/rendering, tools-dialog UX and UI integration tests
passed before activation. Restarted on 9 November 2026 in Screen `15976.nchat`,
window `0`, original shell `15977` and terminal `ttys002`. The new core PID was
`34436`, the Screen frame reported Online, and startup diagnostics contained no
fatal or draft-save failures. The user's actual conversation appearance remains
subject to visual confirmation after scrolling.

### Screen freeze during scrolling

On 8 November, Screen 5.0.2 consumed 100% CPU in
`DoCommandStuff → WinProcess` after its 4096-byte keyboard buffer filled. The
core waited in curses terminal output. Bounded Screen queries also stalled.
Recovery cleared pending keys, released the stuck dispatch and ultimately
restarted the core cleanly in the original session/window. Earlier recovery used
LLDB and a build-specific input-queue address. Such addresses require fresh
derivation and an explicit recovery plan before touching process memory.

The retained workaround is `utils/screen-scroll.conf`, sourced by
`/Users/bingl/.screenrc` and applied to the live session. It bypasses default
`stuff` translations for arrows and Page Up/Down. Kitty and Screen agree on
these sequences. Home/End retain translation because their sequences differ.
Screen copy-mode mappings are separate. The synthetic stress test reproduces
the default freeze and confirms that the workaround services a control request
under the same input flood. Excess keys can still be dropped when buffers fill,
and other `stuff` bindings retain the upstream no-progress-loop risk.

The Screen config also disables visual bell capabilities for `xterm-kitty`
with `termcapinfo xterm-kitty 'vb@:VR@:VN@'`, because a resize could leave a
whole-screen visual flash inverted.

### Unicode drawing and corruption across the sidebar

Long names previously clipped flag emoji between their regional indicators,
creating a terminal/curses width mismatch. Skin-tone emoji also confused Screen
during incremental scrolling. `UiCursesText` now centralizes affected UI drawing,
while flag-safe trimming/wrapping and display-column composer positions cover
the related paths. Inside Screen, supported skin-toned wide-base emoji display
in the base emoji's default colour. Original message/reaction data keeps the
tone. Outside Screen, the grouped tone remains visible. Complete Unicode
grapheme shaping exceeds this fix's implemented coverage.

Implementation commits are `18d4e8b7` for the rendering follow-up and `d473036f`
for the Screen key workaround. Earlier `ae180370` covered only the history glyph
path. Its initial terminal verification was superseded after fixing the test.

### Cache deadlocks and stale state

An earlier real freeze involved a cache callback holding the database mutex while
waiting for UI logic that needed that same cache. Callbacks must run outside the
database lock. Shutdown must drain queued writes before destroying their users.
Relevant regressions exercise real cache reentrancy and shutdown ordering.
Increasing SQLite timeout alone cannot resolve this lock inversion.

Unread state lives beyond loaded history. Preserve the independent pending
ledger, topic/chat isolation, stale-update handling and authoritative membership,
mute and archive state. Late posts from a left channel should stay suppressed
while previously cached history remains available to deliberate cache tools.
Read markers from another device must retire only messages covered by that marker.

Contact names require startup and live updates. Known names should survive an
incomplete later contact payload. WhatsApp PN/LID identities need verified
matching rather than number-based guesses. Live group joins and renames must
publish titles and refresh the visible list while preserving unread state.

### Feature limits that remain relevant

- Historical WhatsApp receipt and poll-voter lists depend on actual supplied
  phone/history records. One live receipt example recovered additional delivery
  times and one poll recovered seven active voters, but complete history remains
  partial. Requests use millisecond cache timestamps converted correctly and a
  real later anchor when recovering a selected cached message.
- WhatsApp Channels have partial live verification. A public channel returned
  14 posts, followed-list lookup returned zero channels for that account, and an
  older-page test returned no posts with a separate metadata lookup returning
  405. Public browsing success does not verify channel publishing/admin actions.
- Telegram per-message editability uses native permissions rather than a blanket
  48-hour rule. Preserve drafts on denial, failure or navigation changes.
- Telegram topics, comments, stories, Secret Chats and many administration/send
  actions have implementations with varying offline/source-review coverage.
  Their actual limitations are recorded in the inventory below and `changes.md`.
- WhatsApp last seen requires active presence/subscriptions and data the service
  actually supplies. Reconnects reapply availability and the active peer
  subscription. Hidden/unavailable last-seen values remain unknown.
- Widget send/read sequencing previously lost send completions or read the wrong
  unread snapshot. Preserve the single composer busy guard, exact send-time IDs
  and acceptance-before-read ordering. The user confirmed the repaired CUMSA
  Mark as read behavior in Telegram. That confirmation applies to that case.
- Full image opening uses original cached files with bounded, validated paths.
  The helper routes detected video to QuickTime because Quick Look's video player
  had closed while opening notes. Codec support and complete downloads still
  affect playback. Images use terminal rendering with `P` for Preview where
  configured.
- Recognized view-once media carries a label and suppresses automatic preview
  and download in terminal/widget/peek. Ctrl+V is the explicit opener. Missing
  WhatsApp payloads can remain phone-only.
- The isolated Android login probe has web, android, android-qr9 and
  android-compat controls. Standard web authentication succeeded in an earlier
  live test. Android attempts reached pairing, a bounded 516 reconnect and 401
  rejection. New compatibility flags passed synthetic checks and lack a verified
  live success. The probe uses a fresh bounded private profile and inspects
  delivery metadata. Universal view-once viewing or receipt bypass is unverified.

## Outstanding request at handover

The user most recently requested continuous presence polling for roughly the
last 50 private contacts on both services, persisted as an activity history
while nchat runs, with hidden statuses skipped. No implementation had been
made at the time of this handover. The user approved the feature on
9 November 2026; it is implemented on this branch as continuous background
presence polling for the most recent 30 private chats per account (see the
`Continuous background presence history` row in `changes.md` and
`doc/custom-settings.md` for the settings and history format). Ordinary
active-chat presence refresh also still exists.

Older birthday-gift research and individual chat-deletion tasks belonged to
separate user requests. They confer no authorization for new message mutations
during maintenance or handover.

## Completing a future feature

1. Record the user-visible outcome and normal entry point. Trace every affected
   service, cache, widget and installed helper caller.
2. Preserve existing data created by earlier builds. Version schema/state
   changes, preserve known metadata when new data is incomplete, and handle
   retries, cancellation, concurrent callbacks and stale responses.
3. Keep F3 text-first, with optional filters after results. Keep tools focused on
   understandable actions, review mutations, preserve drafts on failure and
   avoid making routine lookup depend on unnecessary forms.
4. Keep unread previews inline in the existing popover. Preserve fixed width,
   animated bounded height, compact collapsed layout, visible Back navigation,
   outside-click dismissal and collapsed state on reopen. Full text selection,
   native copy and links belong in the AppKit responder chain.
5. Build the affected core/protocol/widget components and run relevant existing
   checks. Add a targeted regression when existing coverage would miss the bug.
6. Deploy the tested binaries or helper copies to their actual live destinations,
   restart the needed components under the standing authorization, and verify
   identity, location, connection and responsiveness.
7. Make a focused implementation commit, then update `changes.md` with its exact
   link and verification scope. Update README/key/config examples and this
   handover when access, deployment or important constraints change.
8. Push to `origin/codex/personal-features`, verify the remote SHA and a clean
   working tree, and report remaining limitations. Distinguish local cache
   success from a confirmed remote action.

For rollback, `git revert` is appropriate for a compatible isolated feature
commit. Review later fixes and shared prerequisites first. The older feature
history was reconstructed and some features share implementation commits.
Create a dedicated rollback branch and build/test the integrated result.
Use the annotated checkpoints for a complete source restore. Account databases,
local config, installed apps and loaded Screen settings need their own recovery
plan. The checkpoints contain source history rather than a backup of private
runtime state.

## Edited-message synchronization follow-up

Telegram uses `getMessage` for exact content/edit updates. These notifications
set `contentUpdate`, which persists the row without declaring a contiguous
history page. Preserve existing sequence boundaries, sent time and read state.
Ordinary nonsequence display messages, including sponsored entries, retain their
previous non-persistence behavior. Telegram opening refreshes the latest 50 rows,
and cached pagination also queries TDLib to reconcile content.

WhatsApp protocol edits must carry the original key ID and
`EditAttributeMessageEdit`. Unwrapped `events.Message.IsEdit` also sets that flag.
The shared `CacheUtil::PreserveEditedContent` prevents unedited replay from
reverting known edited content in both the durable cache and UI. It allows a
real deletion and a later explicitly edited version to replace content.

Run `sh utils/test-notifications.sh`, `sh utils/test-telegram-edit-permissions.sh`
and `sh utils/test-whatsapp-group-names.sh`. The latter includes edit-event race
fixtures. All tests use synthetic messages and never send service edits. There
is no complete historical recovery guarantee for WhatsApp, or version ordering
between two already-edited snapshots without provider edit-version metadata.

Activated on 9 November 2026 after full core/protocol builds and the notification,
rendering, UI integration, Telegram callback and WhatsApp race suites passed.
The clean restart reused Screen `15976.nchat`, window `0`, shell `15977` and
`ttys002`. The resulting core PID was `31298`. Draft-save and fatal startup
failures were absent from startup diagnostics. A live service edit roundtrip
remains unverified. For already stale Telegram content, reopen the chat and
scroll to the relevant page to initiate reconciliation.

## Feature inventory and access

The `nchat-presence` helper now opens a current-status browser by default.
It combines matching people into one row with separate Telegram/WhatsApp columns,
using complete cached phone numbers or the same service identity across accounts.
For missing metadata, m opens a cached-private-contact picker. Type a name or
number immediately, use arrows to choose, and Enter once confirms a local link.
Escape cancels. Cached counterparts without observations show No report. u removes manual links. Private display preferences live in
`<config>/presence-contact-links.json`, with atomic writes and a separate lock.
Same-name contacts require explicit confirmation. Search includes linked aliases
and complete cached numbers, including formatted number queries.
Its source is `utils/nchat-presence` and its installed entry point is
`/Users/bingl/.local/bin/nchat-presence`. Use `current --plain` for grouped text.
Tab changes service, / or Ctrl+F (also f) searches names/numbers, r reloads the
cache and q quits. Search filters while typing. Arrow/page keys leave text-entry
mode and navigate results.
The view labels stale reports after five minutes and ignores heartbeat records
for freshness. Regression coverage is `python3 utils/test-presence-current.py`.
The weighted acceptance review is `doc/nchat-presence-ui-review.md`. This helper
was upgraded directly with a private recovery copy under
`~/.local/.nchat-helper-install/backups`. The portable helper installer's target
list currently omits `nchat-presence`, so review and deploy its installed copy
explicitly when editing it.

This compact inventory was copied from the `changes.md` feature table at source
`9e4bc1a3`. Each status preserves the recorded verification level. “Offline
verified” refers to the tested synthetic paths. “Live unverified,” “Partial,”
“Source reviewed” and “Config-dependent” retain their specific meanings and
must remain visible when reporting what works. Commit IDs can be inspected with
`git show <commit>` or the matching linked row in the full inventory.

On this Mac, Option means Alt and F-keys may require Fn. Select a message before
using a message action. Common controls include Tab/Shift-Tab for next/previous
chat, Ctrl+Q to quit, Ctrl+V for an attachment, Option+M for receipts,
Option+O for polls and Option+Shift+S for reaction identities. Actual bindings
come from `/Users/bingl/.config/nchat/key.conf`. The README's Ctrl+A/Ctrl+E
line-start/end descriptions should be checked against the active config, whose
home/end actions currently use KEY_HOME/KEY_END. Ctrl+C clears the composer.

In the widget, Command+Enter sends and Control+Command+Enter sends then marks the
captured chat messages read after acceptance. Plain Enter inserts a newline.
Command+A selects the reply draft and Command+C copies selected preview text.
Live clipboard/browser verification has narrower coverage than the synthetic
keyboard wiring tests.

<!-- The compact table below is a handover snapshot, not a second live status store. -->

| Feature | Recorded status | Access | Implementation commits |
|---|---|---|---|
| Widget combined unread badge | Offline verified | Open **nchat Unread.app** | `49ee3fc8` |
| Widget exclude muted chats | Offline verified | Automatic | `41cb5fff`, `a7142a8f` |
| Widget exclude archived chats | Offline verified | Automatic | `ffa3e94e` |
| Widget exclude left chats | Offline verified | Automatic | `c0208073` |
| Widget inline unread previews | Offline verified | Click a chat row | `d7fc7476` |
| Widget stable animated layout | Offline verified | Expand/collapse / Back | `d7fc7476` |
| Widget dismissal and reset | Offline verified | Outside click / Escape / × | `d7fc7476`, `4c333c8f` |
| Widget images and full Preview | Live unverified | Expand → click image | `87ad00d0`, `131e2d68` |
| Widget replies | Live unverified | Expand → Reply | `8b3a717f`, `e165e620` |
| Widget keyboard reply shortcuts | Offline verified | ⌘Enter sends / ⌃⌘Enter sends and mark as read / plain Enter: newline | `94b4bb33`, `8716d0f9`, `5d5d4b2d` |
| Widget Select All | Offline verified | ⌘A in the reply text box | `eae5b4e4` |
| Widget Clear | Offline verified | Expand → Clear | `de9bd9e9` |
| Widget Mark as read | Telegram user-confirmed | Expand → Mark as read | `de9bd9e9`, `e165e620`, `08a9dc7e`, `9ea884d7` |
| Widget poll voting and results | Live metadata lookup verified / live voting untested | Expand unread chat → Vote / results → choose → Vote / Refresh results / Clear selection then Remove vote where permitted | `93188534`, `4f701629`, `f93944f3` |
| Widget copy and links | Live unverified | Select text → ⌘C / click link | `22592736` |
| Widget native WhatsApp reconciliation | Config-dependent | Automatic when that local database is available | `4c333c8f` |
| Widget diagnostics and registration | Offline verified | `utils/build-menubar.sh` / repair utility when needed | `4c333c8f`, `c5eacf4c` |
| Notifications independent ledger | Offline verified | Automatic | `ca1a2ad3`, `7d6462ce`, `147db00a` |
| Kitty Dock badge | Config-dependent | Optional Kitty helper/config snippet | `7d6462ce`, `147db00a` |
| Cross-device read synchronization | Live unverified | Automatic | `e02d54e5` |
| Archive/mute metadata synchronization | Live unverified | Automatic | `12e0c5a5`, `a7142a8f` |
| Left Telegram chats | Offline verified | Automatic | `1fe7e8da` |
| Telegram forum read markers | Live unverified | Automatic | `663435d2` |
| Detached sync mode | Offline verified | `NCHAT_NO_AUTO_SELECT=1 nchat` | `89e57b4c` |
| WhatsApp live group names | Offline verified and activated | Automatic when added to a WhatsApp group or when its title changes. | `284188b1` |
| Contact names and full numbers | Live cache recovery verified | Automatic | `4774ce81`, `12b60730` |
| WhatsApp connection version compatibility | Live reconnect verified | Automatic at process startup | `479e4179` |
| WhatsApp last seen and presence refresh | Activated / live peer display unverified | Open an individual WhatsApp chat / read the status line beside its name. Return to nchat or reopen the chat to refresh. | `f17bb805` |
| Telegram per-message edit permissions | Offline verified / activated | Select a message → Ctrl+Z with the current key configuration / edit and send normally, or cancel to restore the draft. | `bc96fbd0` |
| WhatsApp presence after automatic reconnects | Offline verified / live presence received | Open an individual WhatsApp chat / its online/last-seen information appears in the bottom chat status bar when supplied. | `01901d24` |
| Retained deleted Telegram messages | Offline verified | Optional `message_delete=3` / see settings guide | `297565fd`, `369e7555`, `147db00a`, `a6e4b74e` |
| Screen scrolling input freeze | Offline reproduced and workaround activated | Source `utils/screen-scroll.conf` from `~/.screenrc` / loaded into existing Screen 15976.nchat/window 0 and added to this user’s startup configuration. | `d473036f` |
| Unicode rendering during scrolling | Offline verified and activated | Automatic / normal scrolling and text entry. | `ae180370`, `18d4e8b7` |
| Terminal image thumbnails | Config-dependent | Automatic with installed thumbnail helper | `74e0738a`, `7457364d` |
| Whole wrapped links | Config-dependent | ⌘-click or double-click with snippet enabled / Ctrl-W also available | `74e0738a`, `a5225224` |
| Native/terminal file preview | Config-dependent | Select attachment → Ctrl-V / image viewer → P | `d94cd303`, `7457364d`, `6d432937` |
| Per-person receipts | Partial | Option-M / F3 → preview → Recover message details | `5ed374db`, `8173b530`, `d4cf30dc`, `9b155705` |
| Reaction identities | Partial | Select message → Option-Shift-S | `d247ef14` |
| Rich media labels | Source reviewed | In message history | `f038b623` |
| Poll rendering | Offline verified | In message history | `2d70cb22`, `cab99b0f` |
| Poll voting | Live unverified | Select poll → Option-O → choose options → Submit / Clear selection then Submit withdraws where permitted | `36dff972`, `cab99b0f` |
| Poll voter lists | Partial | Option-O → Refresh / optional F3 cached recovery | `e059c936`, `cab99b0f`, `8173b530`, `c9ae8e9e` |
| Cached attachment browser | Live unverified | Option-Shift-F | `3a93d906` |
| On-device transcription helper | Live unverified | Selected audio/video → Option-Shift-T | `1f12349f`, `84e7b4c7` |
| Read-only all-chat peek | Config-dependent | Installed `nchat-peek` | `131e2d68`, `7457364d` |
| Portable helper packaging | Offline verified | utils/install-macos-helpers.sh --dry-run / --upgrade --only NAME / --replace-reviewed NAME for reviewed legacy files | `369e7555`, `7457364d` |
| Unified tools UI | Offline verified | F2 / form Ctrl-N adds newline, Enter saves field, Option-V pastes | `fc4dd3eb`, `fdd73db1`, `a5511765` |
| Persistent textbox drafts | Offline verified | Automatic | `34627c18`, `ecfd6e72`, `6254ecd6` |
| Global cached search | Offline verified | F3 → text → Enter / Filters (optional) on results / also F2 → Search messages across chats | `29e8f729`, `e592fe9b`, `a5511765` |
| Local chat folders | Offline verified | F4 | `e904d3bb`, `6254ecd6` |
| Local bookmarks | Offline verified | F5 / F2 → Bookmark / unbookmark | `4d2f8b83`, `6254ecd6` |
| Native forwarding picker | Partial | Select message → Option-R | `c10845e6`, `68f99199`, `fceb00f4` |
| Multi-message actions | Live unverified | F6 | `29f9f8d0` |
| Background transcription and archive | Live unverified | Option-Shift-T / F2 → Transcription status / Search saved audio transcripts | `52debf8c`, `84e7b4c7` |
| Native recording | Live unverified | F2 → Record a voice message / Record a video note | `2ce61a84`, `df1d2981` |
| Telegram media metadata | Source reviewed | Automatic | `a7f22626` |
| Telegram contact display | Source reviewed | In message history | `4f913988` |
| Telegram maps/venues/story references | Source reviewed | History / Ctrl-W opens a link | `e868c7b0` |
| Telegram forward labels | Source reviewed | In message history | `8d7961c7` |
| Telegram forum topics | Live unverified | Account tools → Chats, topics and Secret Chats → Topics in this group | `e2188563`, `a5511765` |
| Telegram channel comments | Live unverified | Account tools → Selected message actions → Comments / discussion thread | `25830bb3`, `a5511765` |
| Telegram server search | Live unverified | Account tools → Search Telegram history | `21872fe1`, `a5511765` |
| Telegram cloud/topic drafts | Live unverified | Account tools → Chats, topics and Secret Chats → Cloud draft | `72ca3674`, `ecfd6e72`, `6254ecd6` |
| Telegram albums and original attachments | Partial | Account tools → Selected message actions | `e04d74bd`, `a5511765` |
| Telegram silent/scheduled send | Live unverified | Account tools → Create, schedule or send | `aba179d8` |
| Telegram native voice/video-note send | Live unverified | F2 recording workflow | `cee1c346` |
| Telegram poll/quiz creation | Live unverified | Account tools → Create, schedule or send → Create a poll or quiz | `c3b3c54d` |
| Telegram contact sharing | Live unverified | Account tools → Create, schedule or send → Send a contact | `3c56058d` |
| Telegram location sharing | Live unverified | Account tools → Create, schedule or send → Send a location | `86f2a438` |
| Telegram notification/unread controls | Live unverified | Account tools → Chat controls and administration | `0cbb4a38`, `a7142a8f` |
| Telegram translation and bot buttons | Live unverified | Account tools → Selected message actions | `edb5f082` |
| Telegram administration | Live unverified | Account tools → Chat controls and administration | `6278e2ed` |
| Telegram stories | Live unverified | Account tools → Stories | `00da6d82` |
| Telegram Secret Chats | Live unverified | Account tools → Chats, topics and Secret Chats → Start a Secret Chat / Secret Chats on this device | `deafd5a3` |
| WhatsApp original payload support | Offline verified | Underlying account support | `88f506fb` |
| WhatsApp Status | Live unverified | Account tools → Status updates | `a88d297b` |
| WhatsApp Channels | Partial | F2 → Choose account → WhatsApp → Channels | `2b824d8c`, `6f63e883`, `d6a996d5`, `d9c186a0` |
| WhatsApp Communities | Live unverified | Account tools → Communities | `5800b0fa` |
| WhatsApp poll creation | Live unverified | Account tools → Create poll | `e9cbd15b` |
| WhatsApp mute/unread/stars | Live unverified | Account tools | `11423c27` |
| WhatsApp group administration | Live unverified | Account tools → Group information | `85d5668f` |
| WhatsApp contacts | Live unverified | History / Account tools → Message details / Send contact | `60be43ec` |
| WhatsApp locations | Live unverified | Account tools → Message details / Send location | `b1ad3dd3` |
| WhatsApp business buttons | Live unverified | Account tools → Message details and buttons | `0be8ed57` |
| WhatsApp native recordings | Live unverified | F2 recording workflow | `422acc43` |
| WhatsApp delivered view-once media | Partial | Select message → Ctrl+V when payload exists / widget/peek keep preview hidden | `bc86302b`, `9787941c` |
| WhatsApp historical poll recovery | Partial | F3 → cached WhatsApp poll → Recover message details / then Option-O | `20691592`, `8173b530`, `e9cf1b79` |
| WhatsApp earlier-history request | Partial | Account tools → Request earlier history / F3 → preview → Recover message details | `20691592`, `8c511bf6`, `8173b530`, `e9cf1b79` |

## Reference files

The full inventory carries the detailed per-feature checks and service limits.
The project rules and setup references remain useful alongside this operational
snapshot. [Feature inventory](../changes.md), [project rules](../AGENTS.md),
[README and shortcuts](../README.md), [custom settings and installation](custom-settings.md),
[protocol architecture](DESIGN.md), [original testing checklist](TESTING.md),
[isolated probe guide](viewonce-probe.md).

The personal repository, full customization comparison and saved source
checkpoint are available on GitHub.
[Repository](https://github.com/YeoBL/nchat),
[comparison with vanilla](https://github.com/YeoBL/nchat/compare/vanilla-base...codex%2Fpersonal-features),
[source checkpoint](https://github.com/YeoBL/nchat/tree/checkpoint%2Fnchat-2026-11-09).
