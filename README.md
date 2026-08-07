# QTC Terminal 1.0.0

QTC Terminal is a local-first Linux terminal messenger for a USB-connected MeshCore Companion radio.

This source tree is a clean reconstruction from the preserved QTC 2.3.1 release documentation, the 2.3.1 binary's observable database/protocol strings, the public MeshCore Companion Protocol, and the rebuild specification in `docs/REBUILD-SPECIFICATION.txt`.

QTC is independent software and is not an official MeshCore client.

## Main behavior

- One background core owns the USB serial device and SQLite database.
- Running `qtc` attaches a terminal UI to the existing core or starts one automatically.
- `Ctrl+C` or `F8` detaches the terminal while receiving, history, notifications, and sound continue.
- `Ctrl+Q` twice or `qtc shutdown` stops the core and releases USB.
- Direct messages, channel messages, unread state, aliases/favorites, and history are stored locally.
- Exact MeshCore ACK values are persisted and correlated to the original send across retries and restarts.
- Long UTF-8 messages are split into numbered radio-safe parts and reconstructed as one logical message in QTC.
- Contact aliases and favorite groups are editable directly from the terminal UI.
- Channels and favorites are pinned while the route-grouped contact section scrolls.
- Repeaters, rooms, sensors, and unknown infrastructure are separated into Network Nodes.
- Private channels can be created, joined, rotated, left, and invited through selected MeshCore contacts.
- Invitation sending requires a recipient review screen with Cancel selected by default.
- Incoming structured invitations can be joined or ignored without deleting the original message.
- Urgent sends and stored-message pulls take priority over background contact/channel refreshes.
- Send, receive, and ACK updates use small nonblocking IPC deltas instead of reloading and retransmitting the complete application state.
- Waiting-message pushes trigger an immediate pull-until-empty drain, with a 100 ms safety poll in the default fast mode when a push is missed.
- Inbox drain generations prevent an older `NO_MORE_MESSAGES` response from cancelling a newer waiting-message notification.
- Separate receive-instance keys preserve distinct same-second messages even when sender, timestamp, and text are identical.
- Terminal frames are rendered in memory and written only after a visible state change.
- Four persistent retro palettes are included: Green Phosphor, Amber CRT, Midnight BBS, and Mono TTY.
- UTF-8 and emoji names are measured by terminal-cell width so dividers and selections remain aligned.
- Page Up/Page Down scrolls wrapped message history.
- The core suppresses desktop notifications for an open conversation while still showing an in-terminal banner.
- Settings exposes radio name, TX power, zero-hop/flood advertisement, forced message sync, retry policy, and USB reconnect.
- Settings includes visible theme selection and first-connect regional radio presets.
- The radio's own MeshCore contact card can be exported as a `meshcore://` URI to the Linux clipboard.

## Build

Required development packages:

- C11 compiler
- GNU Make
- SQLite 3 development headers and library
- POSIX/Linux development environment

Fedora:

```sh
sudo dnf install gcc make sqlite-devel
make
make test
```

Debian or Ubuntu:

```sh
sudo apt install build-essential libsqlite3-dev
make
make test
```

The binary is produced at:

```text
build/qtc-linux-x86_64
```

See `BUILDING.md` for sanitizer, release, and packaging commands.

## Try demo mode

```sh
./build/qtc-linux-x86_64 --demo
```

Demo mode starts the same background-core/TUI architecture but does not open a radio.

## Use a MeshCore USB Companion

```sh
./build/qtc-linux-x86_64
```

Force a device:

```sh
./build/qtc-linux-x86_64 --device /dev/ttyACM0
```

List likely serial devices:

```sh
./build/qtc-linux-x86_64 --list-devices
```

Print a desktop-oriented udev rule:

```sh
./build/qtc-linux-x86_64 --print-udev-rule
```

Serial group membership may still be required on some systems:

```sh
sudo usermod -aG dialout "$USER"
```

Log out completely and log in again after changing group membership.

## Terminal controls

| Key | Action |
|---|---|
| `Up` / `Down`, `j` / `k` | Move selection |
| `Enter` | Open the selected conversation and begin writing immediately, or confirm the current dialog |
| `m` | Compose a message in the open conversation |
| `F2` / `e` | Edit the selected contact alias |
| `g` | Assign or change the selected contact favorite group |
| `Page Up` / `Page Down` | Scroll the open conversation history |
| `/` | Search channels, favorites, contacts, aliases, and groups |
| `f` | Toggle favorite status for selected person |
| `F4` / `s` | Open/close Settings |
| `F5` | Reconnect the USB radio through the background core |
| `F6` | Open/close Channels |
| `F7` | Open/close Network Nodes |
| `F8` / `Ctrl+C` | Detach this terminal only |
| `Ctrl+Q` twice | Stop the background core and release USB |
| `Esc` | Cancel dialog or return to Messages |

Settings view:

| Key | Action |
|---|---|
| `1`–`4` | Toggle desktop, sound, direct-message, and channel-message notifications |
| `t` | Open the visible Green Phosphor, Amber CRT, Midnight BBS, and Mono TTY picker |
| `5` | Cycle to the next theme |
| `6` | Toggle in-terminal incoming-message banners |
| `7` | Toggle suppression of desktop notifications for the open conversation |
| `8` | Toggle retries for unconfirmed direct messages |
| `9` | Toggle stale-route reset before retry |
| `0` | Toggle SNR/path details in history |
| `,` / `.` | Decrease/increase stored-message poll interval |
| `[` / `]` | Decrease/increase direct-message attempts from 1 to 4 |
| `d` | Change radio advertisement name |
| `p` | Change radio TX power |
| `z` | Send a zero-hop self advertisement |
| `x` | Send a flood self advertisement |
| `c` | Copy this radio's MeshCore contact card to the clipboard |
| `o` | Open first-connect regional radio presets |
| `y` | Force stored-message synchronization |
| `r` / `F5` | Reconnect the USB radio |
| `n` | Test desktop notification |
| `a` | Test notification sound |

Channels view:

| Key | Action |
|---|---|
| `c` | Create private channel |
| `j` | Join from an invitation URI, a raw 32-character hex key, or `Name:key` |
| `i` | Select one or more person contacts to invite |
| `r` | Rotate selected private-channel key |
| `d` | Leave selected channel while preserving local history |
| `v` | Review a pending incoming invitation |

## Command line

```sh
qtc status
qtc shutdown
qtc channel list
qtc channel create "Family"
qtc channel join "meshcore://channel/add?name=Family&secret=..."
qtc channel join "00112233445566778899aabbccddeeff"
qtc channel join "Family:00112233445566778899aabbccddeeff"
qtc channel invite "Family" "Ana"
qtc channel rotate "Family"
qtc channel leave "Family"
qtc test-notify
qtc test-sound
```

Global options such as `--profile` and `--device` may be placed before or after a command.

## Local files

Data:

```text
${XDG_DATA_HOME:-~/.local/share}/qtc-terminal/<profile>/qtc.db
```

Runtime socket and lock state:

```text
$XDG_RUNTIME_DIR/qtc-terminal/<profile>/
```

Fallback runtime state is created in a user-owned directory below `/tmp` when `XDG_RUNTIME_DIR` is unavailable.

The local socket is permission-restricted and the core verifies the connecting user's UID on Linux.

## Notifications and sound

Desktop notification support uses `notify-send` when available. Sound playback tries available desktop/Linux helpers and never blocks message storage.

Fedora:

```sh
sudo dnf install libnotify pipewire-utils
```

Debian or Ubuntu:

```sh
sudo apt install libnotify-bin pipewire-bin
```

Missing optional helpers disable only that integration.

## Long-message compatibility

QTC carries long text as normal MeshCore text messages using a `QTC-LONG/1` application envelope. QTC 1.0.0 rebuilds the numbered parts into one logical message. Other MeshCore clients that do not understand this envelope may display the individual encoded parts.

## Database migration

QTC 1.0.0 detects the preserved 2.3.1 schema-2 layout and transactionally imports:

- contacts and public keys
- local aliases
- favorites and favorite groups
- route state and node types
- unread counts
- channel rows
- direct/channel messages and timestamps
- settings

The original tables are retained as `legacy_*_v2` backup tables. Legacy channel keys were not present in the 2.3.1 SQLite schema, so they are refreshed from the connected radio after startup.

Back up a real profile before first hardware testing.

## Validation status

Automated validation includes strict warning-as-error builds, exact ACK and protocol/frame tests, multipart-message tests, expanded roster/search tests, invitation tests, IPC tests, exact 2.3.1 database migration tests, sanitizer runs, a complete demo-core lifecycle test, and a pseudo-terminal test covering renderer stability, visible theme and preset pickers, Enter-to-write, favorite selection stability, raw-key channel join, contact-card clipboard export, alias/group editing, long-message composition/reassembly, history paging, and device controls.

Real USB radio verification is still required for a production release. See `VALIDATION.md` for the exact remaining hardware checks and `CAPABILITIES.md` for an explicit implemented/partial/missing audit.
