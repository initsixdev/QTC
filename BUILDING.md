# Building QTC Terminal

## Requirements

- Linux x86-64 with glibc for the current release target
- C11 compiler (`gcc` or `clang`)
- GNU Make
- SQLite 3 development headers and library
- `tar`, `sha256sum`, and standard POSIX shell tools for packaging

## Normal build

```sh
make clean
make
```

Output:

```text
build/qtc-linux-x86_64
```

The default release compile flags are:

```text
-std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
-fstack-protector-strong -D_FORTIFY_SOURCE=2
```

Default linker hardening:

```text
-Wl,-z,relro,-z,now
```

## Tests

Run unit and integration tests:

```sh
make test
```

The test target includes:

- new-database migration and deduplication
- exact QTC 2.3.1 schema migration, including retrying the migration
- invitation URI/raw-key/message validation
- IPC framing and parsing
- MeshCore serial/protocol framing, message layouts, exact ACKs, route-reset, preset, and contact-export encoding
- UTF-8 multipart splitting and logical-message reassembly
- roster grouping, favorites, aliases, groups, flood ordering, and search
- background core startup, attach/status, create/invite/rotate/leave/join, and clean shutdown in demo mode
- pseudo-terminal Enter-to-write, favorite stability, alias/group editing, raw-key join, long-message history, themes, presets, clipboard export, paging, and device controls

## Sanitizers

```sh
make sanitize
```

This builds and runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer, then cleans and rebuilds a normal hardened release binary. The command deliberately leaves a non-sanitized binary in `build/` afterward.

## Full release check

```sh
make release-check
```

This performs a clean hardened build, the full test suite, sanitizer testing, and a final normal rebuild.

## Package release artifacts

```sh
make package
```

Artifacts are written to `dist/`:

- `qtc-linux-x86_64`
- `qtc-terminal-1.0.0-linux-x86_64.tar.gz`
- `qtc-terminal-1.0.0-source.tar.gz`
- `CHANGELOG.md`
- `BUILDING.md`
- `RELEASE-MANIFEST.txt`
- `SHA256SUMS`

The package script tests the exact copied standalone binary in demo-core integration mode before generating checksums.

## Reproducible archive metadata

Set `SOURCE_DATE_EPOCH` to normalize archive timestamps:

```sh
SOURCE_DATE_EPOCH=1786032000 make package
```

When a Git commit exists, the package script uses its commit timestamp by default. Otherwise it uses the current time.

The C compiler output itself may still vary across compiler, linker, libc, and SQLite development versions. The release manifest records those inputs.

## Install

System-wide under `/usr/local/bin`:

```sh
sudo make install
```

Staged package install:

```sh
make DESTDIR=/tmp/qtc-package-root install
```

## Alternate compiler

```sh
make clean
make CC=clang
make test CC=clang
```

## Debug logging

```sh
./build/qtc-linux-x86_64 --debug --demo
```

Do not publish debug logs without reviewing them for contacts, messages, device paths, or channel-related data.
