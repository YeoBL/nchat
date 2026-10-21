# Isolated view-once delivery probe

`cmd/viewonce-probe` tests whether a newly linked experimental client receives
view-once photo metadata. It is **not a media downloader or player**, cannot
recover a missing photo from its placeholder, and does not guarantee that
WhatsApp will supply the content. Existing nchat sessions are not used or changed.

## Build and use

From `lib/wmchat/go`, build into a private location outside the repository:

```sh
go build -o '<private-binary-path>' ./cmd/viewonce-probe
'<private-binary-path>' \
  --probe-dir '<new-absolute-system-temp-path>/nchat-viewonce-probe-session' \
  --sender 'PHONE@s.whatsapp.net' --sender 'LID@lid' \
  --lifetime 20m
```

Replace every placeholder. Supply only the consenting test sender's known phone
and/or LID aliases; repeat `--sender` as needed. The destination must not exist,
must be inside a system temporary directory, and its final directory name must
start with `nchat-viewonce-probe-`. Existing profiles cannot be imported or resumed.

Open the private loopback QR page printed to standard error and link the temporary
device using WhatsApp's **Linked devices** screen. The page refreshes pairing QR
codes automatically and hides them after pairing. Do not share the QR or its URL.

Wait for **Connected**, then have the consenting sender send a **fresh, harmless
test photo** as view-once in a direct chat. Do not open it during this delivery
test. Old restricted messages are not a substitute for a fresh test.

Standard output contains only timestamps and Boolean metadata for fresh incoming
direct messages from the selected aliases: unavailable/view-once/image flags and
URL/path/key/hash presence. Key and hash flags require 32-byte values; they do not
prove the media is downloadable. No message text, identities, URLs, keys, hashes,
or image bytes are printed.

## Safety and limits

- The experimental registration advertises Android-phone capabilities with an
  independent Chrome QR client type. This is an experiment, not confirmed service
  compatibility or a production nchat setting.
- The probe issues no explicit history requests, user-media downloads, presence,
  read receipts, or opened/played receipts. It does not display any photo.
- Ordinary library protocol traffic still occurs: delivery acknowledgements,
  pairing/prekeys, account-state synchronization, and automatic unavailable-message
  requests to the primary phone. Sender filtering limits output, not the underlying
  transport. Normal session/account-state data may be stored in the isolated DB.
- The new directory is private (`0700`); session and QR files are private (`0600`).
  Treat them as account credentials: never publish or commit them.
- Lifetime defaults to 20 minutes; `--lifetime` accepts 1–60 minutes. Ctrl-C or the
  deadline disconnects the probe and closes its DB. It does **not** log out/unlink
  the temporary device or delete the session directory. Afterwards, remove that
  temporary device in WhatsApp and dispose of only its private probe directory.

Offline checks, using synthetic data only:

```sh
go test -race -count=1 ./cmd/viewonce-probe
```
