# Architecture

## Process model

One executable supports three roles:

1. Background core
2. Interactive terminal client
3. One-shot command client

The background core is authoritative. It owns:

- the selected serial device
- MeshCore protocol parsing and command sequencing
- the SQLite connection and all writes
- stored-message polling
- outgoing command queue
- notification and sound execution
- local client socket

The TUI owns only presentation and user input. Closing it does not close USB.

## Runtime isolation

Each profile has independent data, configuration, socket, lock, and PID paths. A nonblocking `flock` prevents two cores from owning one profile. The socket directory and files use restrictive permissions. Linux peer credentials reject clients belonging to another UID.

## IPC

Frames use:

- 4-byte little-endian payload length
- 1-byte message type
- typed payload

State snapshots are a sequence:

- `STATE_BEGIN`
- zero or more contact/channel/message/invitation frames
- settings and status
- `STATE_END`

The core sends snapshots only after meaningful state/revision changes or explicit synchronization. Each attached TUI also reports its active conversation so the core can suppress redundant desktop notifications without suppressing terminal banners.

## Database

SQLite uses WAL mode, foreign keys, a busy timeout, and transactional schema migration. Message deduplication is enforced by a unique stable `message_key`. Logical multipart messages share a `logical_key`; physical segment state and persistent expected-ACK mappings are stored separately so retries and late acknowledgements survive a core restart.

The QTC 2.3.1 importer renames source tables to `legacy_*_v2`, creates the new schema, imports all supported fields, and records schema version 9 in one transaction.

## Roster

The roster model is independent from terminal rendering. It produces:

Pinned:

- configured channels
- favorite groups and favorite people
- Contacts heading

Scrollable:

- direct people
- one-hop people
- increasing hop groups
- flood people

Infrastructure node types are excluded and rendered separately.

## Terminal rendering

The TUI keeps selection, search, contact scroll, history scroll, view, modal, banner, and input state separately from daemon state. A complete character frame is built in memory and emitted in one write after visible state changes. Modal transitions add a short Enter guard so the Enter that opens a confirmation cannot also activate Send.

## Protocol adapter

`protocol.c` contains byte-level MeshCore framing and payload layouts. The rest of the application consumes typed `qtc_radio_event` values and does not parse serial bytes directly. Direct sends retain the firmware-provided 32-bit expected ACK and suggested timeout; the core persists that mapping, handles late ACKs, and optionally resets a stale route before a retry. Long text uses a QTC application envelope over ordinary MeshCore text frames and is reassembled above the protocol adapter.
