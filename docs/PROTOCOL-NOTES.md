# MeshCore Companion Protocol notes

The implementation uses the public Companion Radio Protocol and isolates it in `src/protocol.c`.

USB framing:

- app to radio: `<`, little-endian uint16 payload length, payload
- radio to app: `>`, little-endian uint16 payload length, payload

Startup and synchronization use:

- Device Query, target protocol version 3
- App Start, app protocol version 3
- Get Contacts
- Get Channel / Set Channel
- Sync Next Message

Messaging uses:

- Send Text Message with stable sender timestamp, attempt byte, six-byte destination key prefix
- Send Channel Text Message with channel slot and sender timestamp
- v3 direct/channel receive frames
- message-waiting push
- send-confirmed push

The protocol constants and byte layouts were checked against the MeshCore project Companion Radio Protocol documentation available on 2026-08-06 and against the preserved QTC 2.3.1 binary's observable command/database strings.

Upstream documentation:

https://docs.meshcore.io/companion_protocol/
https://github.com/meshcore-dev/MeshCore/wiki/Companion-Radio-Protocol

The upstream wiki warns that its old embedded copy may be outdated. Real firmware compatibility testing remains part of the release checklist.
