# One source tree, native binaries on macOS and Linux

Date: 2026-08-07
Status: approved

## Goal

`make` produces a working native QTC binary on both macOS and Linux from a single
source tree, with no separate Mac fork and no behavior change on Linux.

A single *binary file* cannot run on both systems — macOS executes Mach-O and Linux
executes ELF. The deliverable is one source tree, two native builds.

## Scope

Full application parity. Everything a user touches works natively on both platforms:
build, tests, radio discovery, clipboard, desktop notifications, notification sound,
IPC peer verification, and background-core auto-spawn.

Release packaging (`packaging/package-release.sh`, `dist/` artifacts, `SHA256SUMS`)
stays Linux-only; it leans on GNU-only `sha256sum`, `tar --sort/--owner`, `date -d`,
and `ld --version`. `make install` is made portable because it costs two lines.

## Design

### 1. Build system

`Makefile` branches on `uname -s`. Linux `CPPFLAGS`, `CFLAGS`, and `LDFLAGS` stay
byte-identical to the current release so recorded build flags do not drift:

    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
      OS_TAG   := macos
      CPPFLAGS ?= -Iinclude -D_DARWIN_C_SOURCE
      LDFLAGS  ?= -Wl,-dead_strip
    else
      OS_TAG   := linux
      CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
      LDFLAGS  ?= -Wl,-z,relro,-z,now
    endif
    BIN := build/qtc-$(OS_TAG)-$(shell uname -m)

On Linux x86-64 this resolves to the existing `build/qtc-linux-x86_64`. On Apple
Silicon it is `build/qtc-macos-arm64`.

`-D_POSIX_C_SOURCE=200809L` is the reason most of the macOS build failures occurred:
on Darwin it *hides* `flock`, `LOCK_*`, and `MSG_DONTWAIT`, which are all present.
`-D_DARWIN_C_SOURCE` exposes them.

`CFLAGS` are shared. Apple clang accepts `-fstack-protector-strong` and
`-D_FORTIFY_SOURCE=2`. The Apple linker rejects `-Wl,-z,...`, hence the branch.

`install` drops GNU-only `install -D` for `mkdir -p` plus `install -m755`.

### 2. `include/qtc/platform.h`

A `MSG_NOSIGNAL` fallback and three declarations. Nothing more.

### 3. `src/platform.c` — the only `#ifdef __APPLE__` in the tree

| Function | Linux | macOS |
|---|---|---|
| `qtc_platform_self_path()` | `/proc/self/exe` | `_NSGetExecutablePath` + `realpath` |
| `qtc_platform_peer_uid()` | `SO_PEERCRED` | `getpeereid()` |
| `qtc_platform_secure_random()` | `getrandom()` | `getentropy()`, chunked to its 256-byte cap |

Two of these fix live defects rather than merely porting:

- `main.c` falls back to `realpath(argv0)`, which yields a bare `"qtc"` when invoked
  from `PATH`. `qtc_spawn_detached` uses `execv`, which does not search `PATH`, so an
  installed macOS build could not auto-spawn its background core.
- `ipc.c` guards the peer check with `#ifdef SO_PEERCRED`. Darwin lacks that constant,
  so the check compiled to an unconditional accept, silently voiding the UID
  verification the README advertises.

### 4. Portable rewrites needing no `#ifdef`

Doing these the portable way behaves identically on both platforms:

- `accept4(fd, SOCK_CLOEXEC)` becomes `accept()` plus explicit `fcntl(FD_CLOEXEC)`.
- `socket(..., SOCK_CLOEXEC|SOCK_NONBLOCK, ...)` becomes `socket()` plus explicit
  `fcntl` (two call sites in `ipc.c`).
- `flock`, `LOCK_*`, and `MSG_DONTWAIT` need only the `_DARWIN_C_SOURCE` from §1.

### 5. Runtime probing for behavioral differences

The codebase already tries candidate helpers until one succeeds. macOS entries are
appended to the existing lists; a non-existent command or a non-matching glob costs
nothing on the other platform.

- `serial.c` device patterns gain `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`,
  `/dev/cu.wchusbserial*`, `/dev/cu.SLAB_USBtoUART*`. Callout (`cu.*`) devices only,
  never `tty.*`: the `tty.*` node blocks on carrier detect and is the wrong device for
  a modem-style radio. The patterns are specific enough to exclude
  `/dev/cu.Bluetooth-Incoming-Port`.
- `tui.c` clipboard helpers gain `pbcopy`.
- `notify.c` sound helpers gain `afplay`. Notifications try `terminal-notifier`, then
  `osascript`.

Message text is remote-controlled, so it is never interpolated into AppleScript source.
The text is passed as argv:

    osascript -e 'on run argv' \
              -e 'display notification (item 1 of argv) with title (item 2 of argv)' \
              -e 'end run' -- <body> <title>

No escaping, no injection surface.

`--print-udev-rule` gains a macOS branch stating that no device rule is required.

### 6. A real bug, fixed unconditionally

Darwin accepted sockets inherit `O_NONBLOCK` from the listener; Linux ones do not.
Combined with the macOS unix-socket buffer default of 8 KB (`net.local.stream.sendspace`,
against roughly 200 KB on Linux), the blocking `qtc_ipc_send` inside `send_snapshot`
hit `EAGAIN` mid-snapshot. `write_all_flags` treats `EAGAIN` as fatal, so the core
dropped the client. Measured: 1 of 10 `demo_core_test.sh` runs passed. Explicitly
clearing the flag took it to 10 of 10.

`accept_clients` therefore sets the accepted descriptor's mode explicitly rather than
inheriting it — a no-op on Linux, correct on both, and it removes the port's dependence
on a subtle platform default. `SO_SNDBUF` is raised on accepted sockets so macOS does
not stall the event loop far more often than Linux during snapshot sends.

### 7. Documentation

Full pass: README positioning plus its build, device, notification, and clipboard
sections; BUILDING.md requirements, per-OS flags, and binary names; and a new
`INSTALL-MACOS.txt`.

Superseded after approval: the platform notes written into CAPABILITIES.md and the
per-OS hardware checks written into VALIDATION.md were removed along with those
files, which were deleted from the repository as historical release material. The
surviving macOS hardware caveats live in README.md and INSTALL-MACOS.txt.

## Testing

On macOS: 8 unit tests, `demo_core_test.sh` run 10 times (guarding against the §6
flake specifically), the TUI pseudo-terminal test, the serial latency test, and
`make sanitize` with ASan and UBSan.

## Known limitation

No Linux machine is available in the session performing this work. Linux compiler
flags, binary path, and code paths are kept byte-identical and the §3–§5 changes are
additive, but "Linux still passes" is verified by inspection only. `make test` must be
run on Linux before the port is trusted there.
