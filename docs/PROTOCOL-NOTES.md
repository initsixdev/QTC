# MeshCore Companion Protocol notes

QTC communicates with MeshCore Companion firmware using the public Companion Protocol. Byte-level framing and payload handling are isolated in `src/protocol.c`.

## USB framing

- application to radio: `<`, little-endian `uint16` payload length, payload
- radio to application: `>`, little-endian `uint16` payload length, payload

QTC opens the serial device in raw nonblocking mode and continuously drains available radio frames.

## Session startup

QTC initializes the Companion application session before normal messaging traffic, queries radio/device information, then performs contact, channel, and stored-message synchronization in the background.

Only one request/response transaction is active at a time, while asynchronous firmware pushes can be received at any point.

## Messaging

QTC uses the Companion protocol operations for:

- direct text messages
- channel text messages
- stored-message synchronization
- message-waiting notifications
- direct-message delivery confirmations
- contact and channel synchronization
- radio settings and self advertisements
- export of the radio's own contact information

Waiting-message pushes trigger immediate stored-message retrieval until the radio reports that no messages remain. Interactive sends and receive work are prioritized over background roster refreshes.

## Upstream documentation

- https://docs.meshcore.io/companion_protocol/
- https://github.com/meshcore-dev/MeshCore/wiki/Companion-Radio-Protocol

Protocol compatibility should be checked against current MeshCore firmware when the upstream Companion protocol changes.
