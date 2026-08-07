#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

VERSION=${VERSION:-1.0.0}
BIN=${BIN:-build/qtc-linux-x86_64}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-unknown}
LDFLAGS=${LDFLAGS:-unknown}
DIST=${DIST:-dist}
SOURCE_NAME="qtc-terminal-${VERSION}-source"
BINARY_NAME="qtc-terminal-${VERSION}-linux-x86_64"

[[ -x "$BIN" ]] || { echo "missing executable: $BIN" >&2; exit 1; }
actual_version=$("$BIN" --version | awk '{print $2}')
[[ "$actual_version" == "$VERSION" ]] || { echo "binary version $actual_version does not match package version $VERSION" >&2; exit 1; }

git_commit="uncommitted-source"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git_commit=$(git rev-parse HEAD)
fi

if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
    epoch=$SOURCE_DATE_EPOCH
elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    epoch=$(git show -s --format=%ct HEAD)
else
    epoch=$(date +%s)
fi

compiler=$($CC --version 2>/dev/null | head -n 1 || true)
target=$($CC -dumpmachine 2>/dev/null || echo unknown)
linker=$(ld --version 2>/dev/null | head -n 1 || echo unknown)
sqlite_version=$(pkg-config --modversion sqlite3 2>/dev/null || echo unknown)
build_date=$(date -u -d "@$epoch" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date -u '+%Y-%m-%dT%H:%M:%SZ')

rm -rf "$DIST"
mkdir -p "$DIST" "$DIST/.staging/$SOURCE_NAME" "$DIST/.staging/$BINARY_NAME"

manifest="$DIST/RELEASE-MANIFEST.txt"
cat > "$manifest" <<MANIFEST
QTC Terminal release manifest
Version: $VERSION
Git commit: $git_commit
Compiler: ${compiler:-unknown}
Target: $target
Linker: $linker
CFLAGS: $CFLAGS
LDFLAGS: $LDFLAGS
Build date (UTC): $build_date
SOURCE_DATE_EPOCH: $epoch
SQLite development version: $sqlite_version
Database schema: 10
MeshCore target protocol: Companion Protocol v3
Feature flags: background-core unix-socket sqlite notifications sound banners open-chat-suppression private-channels invitations legacy-v2-import exact-ack retry-state multipart-history device-controls radio-presets export-contact-card priority-radio-queue inbox-generation receive-instance-keys delta-ipc nonblocking-live-ipc serial-burst-drain fast-inbox-fallback async-composer nonblocking-terminal-output advert-feedback
Hardware validation: not performed in build environment; see VALIDATION.md
MANIFEST

# Copy the exact standalone binary first and test that copied artifact.
install -m 0755 "$BIN" "$DIST/qtc-linux-x86_64"
QTC_BIN="$ROOT/$DIST/qtc-linux-x86_64" ./tests/demo_core_test.sh

# Build a source tree without generated objects, release outputs, or repository metadata.
tar \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.git' \
    --exclude='./src/*.o' \
    --exclude='./src/*.d' \
    --exclude='*.core' \
    -cf - . | tar -xf - -C "$DIST/.staging/$SOURCE_NAME"
cp "$manifest" "$DIST/.staging/$SOURCE_NAME/RELEASE-MANIFEST.txt"

# Binary bundle contains the standalone executable and all operator-facing release documents.
install -m 0755 "$DIST/qtc-linux-x86_64" "$DIST/.staging/$BINARY_NAME/qtc-linux-x86_64"
for file in README.md CHANGELOG.md BUILDING.md INSTALL-LINUX.txt VALIDATION.md CAPABILITIES.md LICENSE NOTICE.md PRIVACY.md; do
    cp "$file" "$DIST/.staging/$BINARY_NAME/$file"
done
cp "$manifest" "$DIST/.staging/$BINARY_NAME/RELEASE-MANIFEST.txt"

# Normalize archive ownership, ordering, and timestamps.
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -czf "$DIST/${SOURCE_NAME}.tar.gz" -C "$DIST/.staging" "$SOURCE_NAME"
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -czf "$DIST/${BINARY_NAME}.tar.gz" -C "$DIST/.staging" "$BINARY_NAME"

cp CHANGELOG.md "$DIST/CHANGELOG.md"
cp BUILDING.md "$DIST/BUILDING.md"

(
    cd "$DIST"
    sha256sum \
        qtc-linux-x86_64 \
        "${BINARY_NAME}.tar.gz" \
        "${SOURCE_NAME}.tar.gz" \
        CHANGELOG.md \
        BUILDING.md \
        RELEASE-MANIFEST.txt > SHA256SUMS
    sha256sum -c SHA256SUMS
)

rm -rf "$DIST/.staging"
printf 'Release artifacts created in %s/%s\n' "$ROOT" "$DIST"
