# Building QTC Terminal

## Requirements

One source tree builds native executables for Linux x86-64 with glibc and for macOS.

Build requirements:

- C11 compiler (`gcc` or `clang`)
- GNU Make
- SQLite 3 development headers and library
- standard POSIX shell tools
- `tar` and `sha256sum` for release packaging, which is Linux-only

Fedora:

```sh
sudo dnf install gcc make sqlite-devel
```

Debian or Ubuntu:

```sh
sudo apt install build-essential libsqlite3-dev
```

macOS:

```sh
xcode-select --install
```

The macOS SDK supplies SQLite 3, so nothing else is needed.

## Build

```sh
make clean
make
```

The Makefile selects platform settings from `uname` and names the output for the host:

```text
build/qtc-linux-x86_64
build/qtc-macos-arm64
```

The default build uses strict warnings and common hardening flags. Compile flags are
shared by both platforms; preprocessor and linker settings differ, because Darwin
hides `flock`, `LOCK_*`, and `MSG_DONTWAIT` behind `_POSIX_C_SOURCE` and the Apple
linker rejects `-Wl,-z,...`:

```text
Linux    -D_POSIX_C_SOURCE=200809L   -Wl,-z,relro,-z,now
macOS    -D_DARWIN_C_SOURCE          -Wl,-dead_strip
```

## Tests

Run the complete test suite:

```sh
make test
```

The suite covers the database, message deduplication and multipart handling, invitations, IPC, MeshCore framing and command encoding, roster/search behavior, background-core lifecycle, and terminal interaction.

## Sanitizers

```sh
make sanitize
```

This runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer, then rebuilds the normal executable.

The instrumented pass runs with `QTC_TIMING_SCALE=10`. Sanitizers slow the
SQLite-backed inbox path by roughly twenty times, which no latency budget tuned for an
optimized binary can absorb; a 24-message burst drain measures about 6.6 s against a
0.3 s optimized baseline. That pass still proves no message is lost, and the optimized
rebuild that follows it re-runs the same tests at the real budgets.

## Full release check

```sh
make release-check
```

This performs a clean build, tests, sanitizer validation, and a final normal build.

## Package release artifacts

Release packaging runs on Linux only. It depends on GNU `sha256sum`, `tar --sort` and
`--owner`, `date -d`, and `ld --version`, none of which behave the same way on macOS.
`make package` stops with an explanatory error there. A macOS build is produced with
`make` and installed with `make install`.

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

`dist/` is generated output and should not be committed to Git.

## Reproducible archive metadata

Set `SOURCE_DATE_EPOCH` to normalize archive timestamps:

```sh
SOURCE_DATE_EPOCH=1786032000 make package
```

When the source is inside a Git repository, the package script uses the current commit timestamp by default. Compiler output can still vary across compiler, linker, libc, and SQLite versions; the generated release manifest records the relevant build inputs.

## Install

System-wide under `/usr/local/bin`:

```sh
sudo make install
```

Staged installation:

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

Run QTC in demo mode with debug logging:

```sh
./build/qtc-linux-x86_64 --debug --demo   # or ./build/qtc-macos-arm64
```

When using a real radio, debug output may contain contact names, message metadata, or device paths. Review logs before sharing them publicly.
