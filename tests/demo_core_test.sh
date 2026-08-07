#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QTC_BIN=${QTC_BIN:-"$ROOT/build/qtc-linux-x86_64"}
TMP=$(mktemp -d /tmp/qtc-demo-core-test-XXXXXX)
CORE_PID=""

cleanup() {
    if [[ -n "$CORE_PID" ]] && kill -0 "$CORE_PID" 2>/dev/null; then
        "$QTC_BIN" shutdown >/dev/null 2>&1 || true
        kill "$CORE_PID" 2>/dev/null || true
        wait "$CORE_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

export XDG_DATA_HOME="$TMP/data"
export XDG_CONFIG_HOME="$TMP/config"
export XDG_RUNTIME_DIR="$TMP/run"
mkdir -p "$XDG_RUNTIME_DIR"

"$QTC_BIN" core --demo --foreground >"$TMP/core.log" 2>&1 &
CORE_PID=$!

ready=0
for _ in $(seq 1 100); do
    if "$QTC_BIN" status >"$TMP/status" 2>&1; then
        ready=1
        break
    fi
    sleep 0.05
done
[[ "$ready" == 1 ]] || { cat "$TMP/core.log" >&2; echo "demo core did not start" >&2; exit 1; }
grep -q "QTC core: running" "$TMP/status"
grep -q "Mode: demo" "$TMP/status"

"$QTC_BIN" channel list >"$TMP/channels-before"
grep -q $'^0\tPublic\tpublic$' "$TMP/channels-before"
grep -q $'^1\tFamily\tprivate$' "$TMP/channels-before"

"$QTC_BIN" channel create "Ops Room"
sleep 0.1
"$QTC_BIN" channel list >"$TMP/channels-created"
grep -q $'\tOps Room\tprivate$' "$TMP/channels-created"

"$QTC_BIN" channel invite "Ops Room" Ana
"$QTC_BIN" channel rotate "Ops Room"
"$QTC_BIN" channel leave "Ops Room"
sleep 0.1
"$QTC_BIN" channel list >"$TMP/channels-left"
! grep -q $'\tOps Room\t' "$TMP/channels-left"

"$QTC_BIN" channel join "meshcore://channel/add?name=Joined%20Room&secret=00112233445566778899aabbccddeeff"
sleep 0.1
"$QTC_BIN" channel list >"$TMP/channels-joined"
grep -q $'\tJoined Room\tprivate$' "$TMP/channels-joined"

"$QTC_BIN" channel join "ffeeddccbbaa99887766554433221100"
sleep 0.1
"$QTC_BIN" channel list >"$TMP/channels-raw-key"
grep -q $'\tPrivate-FFEEDDCC\tprivate$' "$TMP/channels-raw-key"

"$QTC_BIN" shutdown
wait "$CORE_PID"
CORE_PID=""

if "$QTC_BIN" status >/dev/null 2>&1; then
    echo "core still running after shutdown" >&2
    exit 1
fi

echo "demo core integration test passed"
