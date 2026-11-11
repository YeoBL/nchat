# nchat presence current status UI scorecard

This review covers the current-status screen approved by the user on 9 November
2026. Its complete criteria set spans data meaning, information hierarchy,
layout, accessibility, interaction, recovery, performance and deployment.

## Scoring method

Weights total 100. Each criterion receives a rating from 1 to 5. A rating of 1
means absent or harmful, 2 means a major usability problem, 3 means usable with
an unresolved material problem, 4 means the specified checks pass with the
recorded limits, and 5 means the specified behaviour is directly covered by the
checks listed. The weighted score is the sum of weight multiplied by rating
divided by 5. Every criterion must reach at least 4, and data meaning, safe
interaction, terminal bounds and exit checks must pass independently of the total.

These are engineering self-assessments supported by synthetic and terminal tests.
The user has not yet confirmed the visual result. Terminal theme, font and
assistive-technology preferences require the user's own acceptance check.

## Refinement passes

| Pass | Findings | Refinement and verification |
|---|---|---|
| Initial current screen | Long identifiers and historical counts dominated the old listing. | Added a name-first grouped view, selected-contact details, relative report ages and keyboard navigation. |
| Compact layout review | The wide key legend clipped Quit on narrow terminals. Duplicate row arrows added clutter. Selected two-line rows could be cut at the viewport edge. | Compact key legends put Quit first, use one selection marker, and keep both selected row lines in view. Tested width/height combinations and native resize. |
| Data meaning review | A heartbeat could look like fresh presence, old offline data needed a label, and a changing clock heading could suggest a new cache read. | Ignore heartbeats for current-state freshness, label reports older than five minutes, and show the actual cache-read time. Exact observation and last-seen times remain distinct. |
| Recovery and delivery review | First-use, failed refresh, theme fallback and installed-file identity needed explicit checks. | Added empty states, retained-view retry feedback, monochrome/NO_COLOR support, piped output, native terminal checks and a larger synthetic cache benchmark. Verified and backed up the installed helper. |
| Paired-person follow-up | Separate service sections duplicated people who use both apps. Name-only pairing could associate different people. | Added automatic complete-phone/account matches, side-by-side service columns and confirmed local links with cancel/unlink. Real SQLite phone/LID fixtures, independent stale/fresh service values, safe failed saves and concurrent preference writers are tested. |

| Search and assisted matching follow-up | Bare slash input obscured search, the Link picker required extra focus/confirmation, and unsampled counterparts were absent. | Labelled name/number search with Ctrl+F, formatted number matching, immediately focused picker, one-Enter confirmation, all cached private counterparts and explicit No report. Fake-terminal and native PTY regressions pass. |

## Weighted criteria

| Criterion | Weight | Rating out of 5 | Acceptance requirement | Evidence and remaining limits |
|---|---|---|---|---|
| Current-state scope | 4 | 5 | Show one person with independently reported Telegram/WhatsApp states. Browsing is read-only, and explicit Link/Unlink saves local display preferences. | File hashes unchanged after real terminal sessions. No activity counts or timeline controls in this screen. |
| Freshness and stale states | 6 | 5 | Distinguish fresh reports, old offline/coarse reports, unconfirmed online reports and future clock mismatches. | Explicit age boundary, stale-online and future-time cases. |
| Unavailable and coarse data | 4 | 5 | Use honest unavailable/recent/week/month labels and preserve service uncertainty. | Hidden-state, recent-state, coarse timestamp and linked-but-unsampled No report fixtures. |
| Heartbeat semantics | 4 | 5 | A cached heartbeat must never renew the observation time or an online claim. | A fresh heartbeat after a 15-minute-old online observation remains unconfirmed. |
| Contact name priority | 5 | 4 | Show a readable cached name prominently with a transparent identifier fallback. | Names lead rows. Full identities move to the selected-contact panel. Name availability depends on the local cache. |
| Identity disambiguation | 3 | 4 | Join complete matching phones or the same service identity. Require confirmation for name-only pairs and retain both service identities. | Duplicate Alex names stay separate until confirmed. Tests cover cached Telegram phones, WhatsApp LID/phone aliases, multiple accounts and manual unlink. |
| Paired service columns | 4 | 5 | Give each person one row with separate Telegram and WhatsApp values and a service filter. | Matching phone data and manual pairs produce one row. Both service columns retain independent freshness and uncertainty. |
| Stable ordering | 1 | 5 | Keep alphabetical rows stable instead of moving them with every report. | Deterministic person-name and identity sorting. |
| Visual hierarchy | 2 | 4 | Separate title, filter, headers, contact rows, details and controls. | Native screen layouts plus inspected synthetic frames. Terminal typography remains theme-controlled. |
| Signal over clutter | 2 | 5 | Keep raw profile strings, repeated IDs, sample counts and heartbeat counts out of primary rows. | Plain-view checks reject sample/heartbeat clutter and raw JID suffixes in rows. |
| Readable timestamps | 4 | 5 | Use relative report ages, Today/Yesterday labels, readable older dates and exact selected details. | Relative-age, yesterday and last-week tests. |
| Timezone clarity | 2 | 4 | Display local timezone in the snapshot heading and exact details. | Timezone-aware formatting. Arbitrary DST changes were not exhaustively simulated. |
| Column alignment | 3 | 4 | Give names useful space and keep Telegram/WhatsApp columns aligned. | Cell-width bounds at widths 1, 12, 26, 40, 80 and 120. Terminal emoji styling is simplified. |
| Responsive width | 4 | 5 | Use one row with two service columns from 45 terminal columns, and complete person cards below that size. | Column/card boundary fixtures and native 80/120-column sessions with 40-column resizing. |
| Responsive height | 3 | 4 | Keep selected content and essential controls visible across terminal heights. | Heights 6, 10, 12, 13, 16, 24 and 40. Tiny sizes offer an explicit resize/quit state. |
| Visible selection | 3 | 4 | Show the selected row and its details without relying on colour. | Reverse-video selection and a single leading selection marker in monochrome fixtures. |
| Colour independence | 3 | 5 | Preserve meaning and navigation without colour, and honour NO_COLOR. | All interaction fixtures run without colour. Text labels convey every state. |
| Unicode legibility | 4 | 4 | Clip by display cells and preserve readable CJK and accents. | Wide-name clipping and emoji fallback checks. Flags render as country letters and modifiers/joiners use stable fallbacks. |
| Terminal text safety | 4 | 5 | Escape control and bidi characters before display. | An escape-sequence contact name cannot inject terminal controls. |
| Keyboard coverage | 4 | 5 | Reach all current-view functions through keys. | Arrows/j/k, page keys, g/G, Tab, search, refresh, Esc and quit paths. |
| Search feedback | 4 | 4 | Support case-insensitive name/number search, visible input mode and useful zero-result feedback. | Labelled main search, Ctrl+F, formatted cached numbers, immediate picker typing, one-Enter linking, duplicate-name and no-match fixtures. Search is substring matching. |
| Filter clarity | 2 | 5 | Filter people by the presence of a service while preserving both columns. | All people to people with WhatsApp to people with Telegram, with an explicit filter banner. |
| Escape and cancellation | 2 | 5 | Clear search/filter state predictably and keep search keystrokes in the search field. | Search-mode input and Escape/reset tests. A 50 ms default escape delay avoids the usual one-second pause. |
| Paging and orientation | 2 | 4 | Support page/first/last navigation and keep the selection in view. | Page, first/last and scrolling fixtures count people instead of physical display lines. |
| Refresh behaviour | 3 | 5 | Show when the cache was read, retain the selected identity and report completion. | Selection-preservation and refresh-result fixtures. The screen reloads on r. |
| Empty states | 2 | 5 | Handle first use and empty filters with clear next actions. | Missing-directory and zero-result tests. |
| Errors and recovery | 3 | 4 | Preserve the last good view on refresh failure and offer a retry. | Injected read failure retains contact details and displays retry guidance. Corrupt-record warnings are reduced to a UI note. |
| Tiny terminal fallback | 2 | 4 | Offer a usable exit when the screen cannot fit. | 20 by 6 and 26 by 10 fixtures keep q visible. |
| Performance and resources | 2 | 4 | Keep rendering local and bounded by contact count, with an explicit refresh budget. | 10,000 cached reaffirmations refresh in about 0.02 seconds on this Mac against a 2-second test ceiling. Loading still scans the input files. |
| Plain output and assistive use | 2 | 5 | Provide labelled text without ANSI/cursor control for pipes and --plain. | Real subprocess pipe test and grouped text formatting. |
| Discoverability | 2 | 4 | Make default launch useful and keep essential help visible. | No-argument launch opens current status. Compact and wide key legends always include Quit. Full keys are also documented. Shared --help also includes legacy command options. |
| Clean exit and resize | 2 | 5 | Restore the terminal and exit without traceback after normal quit, resize or interruption. | Native PTY resize/quit checks and curses-wrapper error handling. |
| Installation and recovery | 1 | 5 | Deploy the tested helper atomically and preserve the replaced helper. | Installed source hash and self-tests are checked. A private recovery copy is retained. |
| Compatibility | 2 | 5 | Keep existing explicit commands available while making the current view the default. | Original synthetic self-test passes. Explicit list/show/summary routing is retained. |

The resulting score is **92.0/100**, with every criterion rated at least
4/5. The score describes the tested current view and the limits above.

## Reproduce the checks

```sh
cd /Users/bingl/.local/src/nchat-inline
python3 utils/test-presence-current.py
python3 utils/nchat-presence --self-test
```

The first script uses temporary synthetic SQLite contacts and JSONL reports. It
checks freshness, identity, search, filters, paging, refresh, failures, compact
layouts, Unicode bounds and plain output. It also opens real pseudo-terminals
at 80 and 120 columns, resizes to 40 columns, exercises keys, quits and verifies
that the cached contact/report source files were unchanged during browsing. Explicit link actions have separate private preference-write tests. Its 10,000-record benchmark keeps seven
latest contact states and is subject to a two-second regression ceiling.
The second script retains the existing helper's explicit-command regressions.

## Usage

Run `nchat-presence` or `nchat-presence current`. Use arrows or j/k to move,
Tab to filter people by service, / to search, Enter to finish entering a query,
Esc to clear, r to read the cache again and q to quit. m opens the other-service
picker and Enter confirms a link. u removes manual links. Page Up/Page Down and g/G move
farther through the list. `nchat-presence current --plain` prints a grouped
snapshot. `NO_COLOR=1 nchat-presence` requests monochrome display.

The five-minute stale threshold is a presentation choice. Report age advances
between refreshes. New cached data appears automatically within about one second
in both the status table and the open activity history. File metadata checks
avoid reparsing unchanged records. Search, selection and the viewed history
period stay in place. Press r to force an immediate reload. Emoji modifiers and joined
styling are simplified for stable terminal columns, while the underlying names
remain intact in the cache.

## Installation verification

On 9 November 2026, the installed helper at
`/Users/bingl/.local/bin/nchat-presence` matched the tested source byte for byte.
Its current plain-view invocation completed successfully against the existing
local cache with output discarded, and its synthetic self-test passed. The
replaced helper is retained privately under
`~/.local/.nchat-helper-install/backups/20261109T103214Z-nchat-presence-current/`.
A source rollback should be followed by deployment of the selected helper copy.

The paired-person update was installed and reverified on 9 November 2026. Its
predecessor is retained under
`.local/.nchat-helper-install/backups/20261109T105728Z-nchat-presence-people` in the user home directory.
The installed source matches the tested file, both column labels load against
the real local cache, and complete-phone matches produce combined rows.

The search follow-up retains the 92/100 self-assessment. Its checks now include
cached private candidates without observations, self/group exclusions, formatted
phone search, a focused picker, one-Enter linking and Escape cancellation. Local
assisted matches are saved outside the repository with a private recovery record.
Their identities and account data are excluded from this document and Git.

On 9 November 2026, the merged search helper was installed atomically and its
bytes matched the source. Installed self-tests and a real-cache plain invocation
passed. The replaced helper is retained under
`~/.local/.nchat-helper-install/backups/20261109T112815Z-presence-contact-search/`.
The assisted preference writes were read back and all planned pairs resolved to
combined rows. Recovery details remain private under the configuration directory.

## History and matching follow-up on 10 November 2026

The earlier numeric score describes the current-status view. The user reported
confusing day offsets in the history view and an incorrect saved match that
made one person's row appear under another name. Those reports are recorded
separately from the earlier self-assessment.

The history now has explicit date columns, seconds, full service names,
readable unknown-end labels, a visible range counter and compact cards below
67 terminal columns. Reload preserves the viewed position. Synthetic checks
cover midnight crossings, 26–120-column layouts and tiny-terminal exit, while
native PTYs open history, resize and return to contacts. The picker omits people
already paired with the source service, and conflicting duplicate endpoints
are rejected under the preference lock. Tests confirm rejected writes preserve
the old preferences. The reported local associations were repaired privately.

Both the expanded UI suite and the original helper self-test pass. This records
the tested changes without assigning a new score or claiming user acceptance of
the revised layout.

The merged helper was installed atomically on 10 November 2026. Its bytes match
the tested source. Installed self-tests and a real-cache plain invocation pass.
A private copy of the predecessor is retained under
`~/.local/.nchat-helper-install/backups/` in the dated presence-history-ux
folder. Existing helper sessions need to be reopened to load the new code.

## Live refresh verification — 11 November 2026

The terminal regression appends an online record while the status table is idle,
then an offline record while that person's history is open. Both changes appear
without a refresh key or restart. Synthetic checks also cover automatic recovery,
selection/history anchors, contact-link and SQLite WAL changes, and skipping
unchanged history. The complete viewer suite and helper self-test pass. The
installed helper matches the tested source. Previously running copies require
one reopen to load this change.
