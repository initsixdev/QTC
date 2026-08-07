# QTC 1.0.0 capability audit

This file separates implemented behavior from work that still requires a physical MeshCore Companion radio. A setting, packet encoder, or demo-only path is not counted as complete unless the background core and terminal UI are wired to it.

## Operational in code

### Core, storage, and lifecycle

- USB serial discovery, explicit device selection, open/close, automatic reconnect, and manual reconnect.
- MeshCore USB framing and Companion Protocol v3 startup, device information, contacts, channel slots, stored-message polling, direct sends, channel sends, and send-confirmation parsing.
- One persistent background core owns the serial device and is the only SQLite writer.
- Permission-restricted Unix-socket IPC with peer-UID validation.
- Automatic core start/attach, `F8` or `Ctrl+C` detach, reattach, and deliberate full shutdown.
- Transactional additive schema migration, including exact import of the preserved QTC 2.3.1 schema-2 layout.

### Messaging reliability

- Stable local message identity and sender timestamp across retries.
- Exact 32-bit expected-ACK correlation from `RESP_CODE_SENT` to `PUSH_CODE_SEND_CONFIRMED`.
- Persistent ACK history, so a late ACK can still mark the correct message delivered after a retry or core restart.
- Settings-driven direct-message retry policy with one to four attempts.
- Optional stale-route reset before a retry.
- Queued retries are skipped when an earlier attempt is confirmed.
- Distinct receive-instance keys prevent two legitimate same-second messages with identical text from collapsing into one database row.
- Stored-message draining while no TUI is attached.
- Persistent pull-until-empty inbox draining after a waiting-message push, with a 100 ms fallback poll in the default fast mode.
- Generation-based inbox state prevents a stale empty response from cancelling a newer waiting-message push.
- Priority radio queue that keeps sends and inbox pulls ahead of background roster refreshes.
- Direct and channel outgoing state aggregation: queued, sending, sent, delivered, unconfirmed, and failed.
- Live send, receive, ACK, and unread changes are published as small record deltas; the core does not reload or retransmit the full application state on the messaging hot path.
- Live broadcasts use nonblocking Unix-socket writes, so a suspended or slow terminal client cannot hold up serial command writes or ACK parsing.
- Serial and IPC readers drain all available frames per wake, including a `SENT` response immediately followed by its ACK push.

### Long messages and history

- UTF-8-safe splitting of text that exceeds the radio frame limit.
- Stable `QTC-LONG/1` multipart envelopes with a logical message token and numbered parts.
- Incoming and outgoing reassembly into one visible logical message.
- One unread increment and one notification per completed multipart message rather than one per physical part.
- Wrapped multi-line history display.
- `Page Up` and `Page Down` history scrolling.
- Optional SNR and route/path metadata in history.

### Contacts, roster, and interface

- Person contacts are separated from repeaters, rooms, sensors, and unknown infrastructure nodes.
- Pinned channels and favorite groups with a scrollable direct/hop/flood contact area.
- Case-insensitive substring and `*`/`?` wildcard search across channel names, aliases, original radio names, favorite groups, and key prefixes.
- Interactive alias editing with `F2` or `e`.
- Interactive favorite toggle with `f`, preserving the selected contact when it moves into or out of Favorites.
- Interactive favorite-group assignment with `g`.
- Enter opens the selected conversation directly in compose mode.
- Four persistent terminal themes with a visible picker: Green Phosphor, Amber CRT, Midnight BBS, and Mono TTY.
- First-connect regional radio preset picker with an explicit local-configuration warning.
- UTF-8/emoji-aware cell layout, absolute-position rendering, stable dialogs, and revision-driven redraws.

### Notifications and device controls

- Desktop notifications and notification sound helpers continue while the TUI is detached.
- The core tracks every attached client's open conversation.
- Desktop notification suppression for a conversation currently open in any attached TUI.
- In-terminal incoming-message banners, including while desktop notifications are suppressed.
- Settings controls for radio name, TX power, zero-hop advertisement, flood advertisement, forced stored-message sync, and USB reconnect.
- Advert actions show immediate "sending" feedback followed by explicit sent, failed, or timed-out completion status.
- Export of the radio's own MeshCore contact card as a standard `meshcore://` URI through Linux clipboard helpers.
- Settings controls for poll interval, retry attempts, retry-unconfirmed behavior, stale-route reset, signal display, notifications, sound, banners, and theme.

### Private channels

- Create, invitation-URI join, raw-hex-key join, `Name:key` join, key rotation, leave, and local history preservation.
- Cryptographically random 16-byte private-channel secrets.
- Searchable multi-contact invitation picker restricted to person contacts.
- Mandatory recipient review with Cancel selected by default.
- Invitations sent as normal MeshCore direct messages.
- Incoming structured invitation recognition with Join and Ignore actions.

## Implemented but still requires physical-radio validation

The following paths are complete in code and covered by parser, database, demo-core, or pseudo-terminal tests, but cannot be declared hardware-proven in this build environment:

- Exact ACK values and suggested timeout behavior on the user's installed Companion firmware.
- Late ACK arrival during an active retry and after a daemon restart.
- Stale-route reset followed by a successful resend without duplicate delivery.
- Long direct and channel messages over real RF links, including missing, delayed, duplicated, and out-of-order parts.
- Device-name and TX-power persistence on the radio.
- Zero-hop and flood advertisements.
- Exported self-contact-card compatibility with other MeshCore clients.
- Regional preset application and returned radio parameters after reconnect.
- Forced message draining and USB unplug/replug recovery under traffic.
- Channel create/join/rotate/leave interoperability with other MeshCore clients.
- Notifications and sound during long detached sessions.

## Deliberate compatibility note for long messages

`QTC-LONG/1` is a QTC application envelope carried inside ordinary MeshCore text messages. QTC 1.0.0 reassembles it automatically. Clients that do not understand this envelope may display the numbered wire parts as ordinary text. No radio firmware modification is required.

## Still not implemented

- Complete slash-command compatibility with every preserved 2.3.1 command.
- F1 full-screen help and F3 raw protocol/event-log views.
- Automatic retransmission of only a missing multipart segment requested by the receiver.
- Production targets outside Linux x86-64 glibc.

QTC 1.0.0 additionally removes blocking full-state work from live messaging, uses nonblocking record deltas, drains serial/IPC bursts, and adds missed-push fallback latency coverage. Real-radio validation remains the final gate for calling radio-dependent paths production-proven.

## Live composer

The message composer is not a modal pause. QTC continues draining background-core updates, displaying incoming messages and delivery-state changes, and accepting keyboard input simultaneously. Terminal output is nonblocking, and the latest complete frame replaces stale pending output without losing the draft.
