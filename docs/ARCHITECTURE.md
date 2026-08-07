# Architecture

## Process model

QTC uses one executable in three roles:

1. background core
2. interactive terminal client
3. one-shot command client

The background core is authoritative. It owns:

- the selected USB serial device
- MeshCore protocol parsing and command sequencing
- the SQLite connection and all writes
- stored-message synchronization
- the outgoing command queue
- desktop notification and sound execution
- the local client socket

The terminal UI owns presentation and user input only. Detaching the UI does not close USB or stop message handling.

## Runtime isolation

Each profile has independent data, socket, lock, and PID state. A nonblocking `flock` prevents two cores from owning the same profile. Runtime files use restrictive permissions, and peer credentials are used to reject local clients from another UID: `SO_PEERCRED` on Linux, `getpeereid` on macOS.

## Core/client IPC

Local frames use:

- 4-byte little-endian payload length
- 1-byte message type
- typed payload

Initial state is delivered as a snapshot. Live sends, receives, acknowledgements, unread counts, settings, and connection changes use incremental updates so normal terminal activity does not block the radio path.

The terminal reports its active conversation to the core so desktop notifications can be suppressed for an already-open chat while in-terminal message feedback remains available.

## Database

QTC uses SQLite in WAL mode with foreign keys, a busy timeout, and transactional schema migrations.

The database stores contacts, aliases, favorites, channels, messages, unread state, settings, invitations, multipart state, retry state, and expected MeshCore acknowledgements. Migrations are designed to preserve existing profile data.

The background core is the only database writer.

## Radio session

The serial connection is opened in raw, nonblocking mode. QTC begins reading the device immediately, initializes the Companion session, then allows normal interactive traffic once the radio session is ready.

Protocol commands are serialized as required by the Companion protocol. Interactive sends and waiting-message retrieval take priority over background contact and channel refresh work.

Asynchronous radio pushes are handled as soon as they arrive. Waiting-message notifications trigger a drain of stored messages until the radio reports that the queue is empty.

## Roster

The roster model is independent from terminal rendering.

Pinned content:

- configured channels
- favorites and favorite groups
- Contacts heading

Scrollable content:

- direct contacts
- one-hop contacts
- increasing hop groups
- flood contacts

Repeaters and other infrastructure nodes are kept out of the normal person-contact list and displayed on the Network Nodes page.

## Terminal rendering

The TUI keeps selection, search, contact scroll, history scroll, current view, modal state, banners, and input state separate from daemon state.

Frames are built in memory and written only when visible state changes. Terminal output and IPC are nonblocking so an active composer or a slow terminal cannot pause message reception.

## Protocol adapter

`src/protocol.c` contains byte-level MeshCore framing and payload layouts. The rest of QTC consumes typed radio events rather than parsing serial bytes directly.

Direct-message acknowledgement state is associated with the original outgoing message so confirmation and retry state remain consistent. Long messages are split above the protocol layer and reassembled into one logical conversation message by QTC.
