#!/usr/bin/env python3
"""Pseudo-terminal regression test for QTC's absolute, UTF-8-aware renderer."""

import fcntl
import os
import platform
import pty
import re
import select
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata

ROWS = 28
COLS = 100
CSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


def cell_width(ch: str) -> int:
    if unicodedata.combining(ch):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def render_terminal(data: bytes) -> list[str]:
    text = data.decode("utf-8", "replace")
    screen = [[" "] * COLS for _ in range(ROWS)]
    row = 0
    col = 0
    pos = 0
    while pos < len(text):
        if text[pos : pos + 2] == "\x1b[":
            match = CSI.match(text, pos)
            if match:
                params, command = match.group(1), match.group(2)
                pos = match.end()
                if command == "H":
                    parts = params.split(";") if params else []
                    row = (int(parts[0]) if parts and parts[0].isdigit() else 1) - 1
                    col = (int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 1) - 1
                elif command == "K" and 0 <= row < ROWS:
                    if params == "2":
                        screen[row] = [" "] * COLS
                    else:
                        for index in range(max(0, col), COLS):
                            screen[row][index] = " "
                continue
        ch = text[pos]
        pos += 1
        if ch == "\r":
            col = 0
            continue
        if ch == "\n":
            row += 1
            continue
        if ord(ch) < 32:
            continue
        width = cell_width(ch)
        if width == 0:
            continue
        if 0 <= row < ROWS and 0 <= col < COLS:
            screen[row][col] = ch
            if width == 2 and col + 1 < COLS:
                screen[row][col + 1] = " "
        col += width
    return ["".join(line) for line in screen]


def read_available(master: int, duration: float) -> bytes:
    output = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return bytes(output)


def default_binary() -> str:
    tag = "macos" if platform.system() == "Darwin" else "linux"
    return f"./build/qtc-{tag}-{platform.machine()}"


def main() -> None:
    binary = os.environ.get("QTC_BIN", default_binary())
    runtime_root = tempfile.mkdtemp(prefix="qtc-tui-runtime-")
    data_root = tempfile.mkdtemp(prefix="qtc-tui-data-")
    profile = f"smoke-{os.getpid()}"
    env = os.environ.copy()
    env["XDG_RUNTIME_DIR"] = runtime_root
    env["XDG_DATA_HOME"] = data_root
    helper_dir = os.path.join(runtime_root, "bin")
    os.makedirs(helper_dir, exist_ok=True)
    clipboard_capture = os.path.join(runtime_root, "clipboard.txt")
    helper = os.path.join(helper_dir, "wl-copy")
    with open(helper, "w", encoding="utf-8") as script:
        script.write('#!/bin/sh\ncat > "$QTC_CLIPBOARD_CAPTURE"\n')
    os.chmod(helper, 0o755)
    env["QTC_CLIPBOARD_CAPTURE"] = clipboard_capture
    env["PATH"] = helper_dir + os.pathsep + env.get("PATH", "")

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    proc = subprocess.Popen(
        [binary, "--demo", "--profile", profile],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
        close_fds=True,
    )
    os.close(slave)
    output = bytearray(read_available(master, 2.0))

    screen = render_terminal(bytes(output))
    assert screen[0].startswith(" QTC TERMINAL 1.0.0"), screen[0]
    assert screen[2].startswith("  CHANNELS"), screen[2]
    assert "| Select a conversation" in screen[2], screen[2]
    assert "F4 Settings" in screen[ROWS - 2], screen[ROWS - 2]
    # The historical bug placed each row at an increasing horizontal offset.
    assert screen[3].startswith(" > Public"), screen[3]
    assert screen[4].startswith("   Family"), screen[4]

    os.write(master, b"\x1b[14~")  # F4 settings
    output.extend(read_available(master, 0.4))
    settings = render_terminal(bytes(output))
    assert settings[2].strip() == "SETTINGS", settings[2]
    assert "Green Phosphor" in "\n".join(settings)

    os.write(master, b"t2")  # visible theme picker, then Amber CRT
    output.extend(read_available(master, 0.5))
    amber = render_terminal(bytes(output))
    assert "Theme [t]: Amber CRT" in "\n".join(amber)

    os.write(master, b"o1")  # first-connect Europe / UK preset
    output.extend(read_available(master, 0.5))
    preset = render_terminal(bytes(output))
    assert "867.5000 MHz" in "\n".join(preset)

    os.write(master, b"c")  # export own MeshCore contact card to clipboard
    output.extend(read_available(master, 0.5))
    assert os.path.exists(clipboard_capture)
    with open(clipboard_capture, encoding="utf-8") as copied:
        assert copied.read().startswith("meshcore://")

    os.write(master, b"\x1b[14~")  # back to messages
    os.write(master, b"\x1b[B\x1b[B")  # Public -> Family -> Ana
    output.extend(read_available(master, 0.4))
    ana_selected = render_terminal(bytes(output))
    assert any(line.startswith(" > Ana") for line in ana_selected), "Ana was not selected"

    # Restore the old interactive conveniences through the actual TUI: favorite, alias, and group.
    os.write(master, b"f")
    output.extend(read_available(master, 0.3))
    os.write(master, b"\x1b[12~")  # F2 alias
    os.write(master, "Mama Ana".encode("utf-8") + b"\r")
    output.extend(read_available(master, 0.5))
    os.write(master, b"g" + b"\x7f" * 32 + b"Trusted\r")
    output.extend(read_available(master, 0.5))

    os.write(master, b"\r")  # Enter opens selected contact directly in compose mode
    long_message = "Long message čćž " + ("0123456789" * 30)
    os.write(master, long_message.encode("utf-8") + b"\r")
    output.extend(read_available(master, 0.8))
    history = render_terminal(bytes(output))
    assert "Mama Ana" in "\n".join(history)
    assert "Long message" in "\n".join(history)
    os.write(master, b"\x1b[5~\x1b[6~")  # Page Up / Page Down history
    output.extend(read_available(master, 0.3))

    # Device controls are exposed in Settings and handled by the core, not only stored labels.
    os.write(master, b"\x1b[14~")
    os.write(master, b"d" + b"\x7f" * len("QTC Demo Radio") + b"Field Radio\r")
    output.extend(read_available(master, 0.4))
    os.write(master, b"p" + b"\x7f" * 4 + b"7\r")
    output.extend(read_available(master, 0.4))

    os.write(master, b"z")
    output.extend(read_available(master, 0.5))
    zero_hop_feedback = render_terminal(bytes(output))
    zero_hop_text = "\n".join(zero_hop_feedback)
    assert "ADVERT" in zero_hop_text
    assert "0-hop advertisement sent" in zero_hop_text

    os.write(master, b"x")
    output.extend(read_available(master, 0.5))
    flood_feedback = render_terminal(bytes(output))
    flood_text = "\n".join(flood_feedback)
    assert "ADVERT" in flood_text
    assert "Flood advertisement sent" in flood_text

    os.write(master, b"yr\x1b[15~")
    output.extend(read_available(master, 0.6))
    device_settings = render_terminal(bytes(output))
    assert "Field Radio" in "\n".join(device_settings)
    assert "TX 7 dBm" in "\n".join(device_settings)
    os.write(master, b"\x1b[14~")
    output.extend(read_available(master, 0.3))

    os.write(master, b"\x1b[17~")  # F6 Channels
    output.extend(read_available(master, 0.3))
    channels = render_terminal(bytes(output))
    assert "j Join" in "\n".join(channels)
    os.write(master, b"j00112233445566778899aabbccddeeff\r")
    output.extend(read_available(master, 0.7))
    joined = render_terminal(bytes(output))
    assert "Private-00112233" in "\n".join(joined)

    os.write(master, b"\x03")  # Ctrl+C detaches the terminal, not the core
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise AssertionError("TUI did not detach after Ctrl+C")
    finally:
        os.close(master)

    db_path = os.path.join(data_root, "qtc-terminal", profile, "qtc.db")
    with sqlite3.connect(db_path) as db:
        alias, group_name, favorite = db.execute(
            "SELECT alias,favorite_group,favorite FROM contacts WHERE name='Ana'"
        ).fetchone()
        assert alias == "Mama Ana"
        assert group_name == "Trusted"
        assert favorite == 1
        rows = db.execute(
            "SELECT text,part_index,part_total FROM messages "
            "WHERE direction=1 AND logical_key LIKE 'out-long:%' ORDER BY part_index"
        ).fetchall()
        assert len(rows) >= 2, rows
        assert {row[2] for row in rows} == {len(rows)}
        assert "".join(row[0] for row in rows) == long_message
        tx_power = db.execute("SELECT value FROM settings WHERE key='tx_power'").fetchone()
        assert tx_power is not None and tx_power[0] == "7"

    subprocess.run(
        [binary, "shutdown", "--profile", profile],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        timeout=5,
    )
    shutil.rmtree(runtime_root, ignore_errors=True)
    shutil.rmtree(data_root, ignore_errors=True)
    print("tui smoke test passed")


if __name__ == "__main__":
    main()
