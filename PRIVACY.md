# Privacy statement

QTC Messenger Terminal is designed to operate locally and offline.

- No online account is created.
- No telemetry, advertising, analytics, crash upload, or automatic update check is implemented.
- Contacts, aliases, unread state, settings, and chat history remain in a local SQLite database.
- Radio traffic is exchanged only with the USB-connected MeshCore Companion.
- The application does not contact the developer or any cloud service.
- `--debug` writes protocol activity to stderr; avoid sharing debug logs without reviewing them.

Delete `~/.local/share/qtc-terminal/<profile>` to remove a profile's local history.
