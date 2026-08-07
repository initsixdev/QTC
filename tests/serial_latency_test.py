#!/usr/bin/env python3
"""Regression test for low-latency serial messaging and nonblocking UI updates."""

from __future__ import annotations

import ctypes
from collections import deque
import fcntl
import os
import platform
import pty
import select
import shutil
import socket
import sqlite3
import struct
import subprocess
import tempfile
import termios
import threading
import time
from pathlib import Path

from tui_smoke_test import COLS, ROWS, read_available, render_terminal

QTC_IPC_HELLO = 1
QTC_IPC_STATE_END = 7
QTC_IPC_MESSAGE = 5
QTC_IPC_SEND_DIRECT = 20
QTC_IPC_STATUS = 8
QTC_IPC_CORE_INFO = 10
QTC_IPC_DEVICE_ADVERTISE = 39
QTC_MSG_INCOMING = 0
QTC_MSG_OUTGOING = 1
QTC_MSG_QUEUED = 0
QTC_MSG_DELIVERED = 3

# The budgets below describe an optimized binary. A sanitizer build cannot meet
# them: the SQLite-backed inbox path runs roughly twenty times slower under
# ASan+UBSan, so a 24-message burst drain measures about 6.6 s against a 0.3 s
# optimized baseline. The sanitize target sets QTC_TIMING_SCALE for its
# instrumented pass only, so that pass still proves no message is lost while the
# optimized run that follows it enforces the real budgets unchanged.
TIMING_SCALE = float(os.environ.get("QTC_TIMING_SCALE") or 1)


class Message(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_int64),
        ("conversation_kind", ctypes.c_int),
        ("conversation_key", ctypes.c_char * 65),
        ("direction", ctypes.c_int),
        ("sender_timestamp", ctypes.c_int64),
        ("attempt", ctypes.c_int),
        ("status", ctypes.c_int),
        ("message_key", ctypes.c_char * 160),
        ("logical_key", ctypes.c_char * 160),
        ("text", ctypes.c_char * 768),
        ("part_index", ctypes.c_int),
        ("part_total", ctypes.c_int),
        ("ack_code", ctypes.c_uint32),
        ("ack_deadline", ctypes.c_int64),
        ("snr_quarter_db", ctypes.c_int),
        ("path_len", ctypes.c_int),
        ("created_at", ctypes.c_int64),
    ]


assert ctypes.sizeof(Message) == 1232


class DeviceAction(ctypes.Structure):
    _fields_ = [
        ("text", ctypes.c_char * 96),
        ("value", ctypes.c_int),
        ("flag", ctypes.c_bool),
    ]


def wire_frame(payload: bytes) -> bytes:
    return b">" + struct.pack("<H", len(payload)) + payload


def ipc_send(sock: socket.socket, frame_type: int, payload: bytes = b"") -> None:
    sock.sendall(struct.pack("<IB", len(payload), frame_type) + payload)


def recv_exact(sock: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise ConnectionError("QTC IPC socket closed")
        data.extend(chunk)
    return bytes(data)


def ipc_recv(sock: socket.socket, timeout: float = 2.0) -> tuple[int, bytes]:
    sock.settimeout(timeout)
    header = recv_exact(sock, 5)
    length, frame_type = struct.unpack("<IB", header)
    return frame_type, recv_exact(sock, length) if length else b""


def c_string(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", "replace")


class RadioSimulator(threading.Thread):
    def __init__(self, master_fd: int) -> None:
        super().__init__(daemon=True)
        self.master_fd = master_fd
        self.stop_event = threading.Event()
        self.send_seen = threading.Event()
        self.send_seen_at = 0.0
        self.incoming_lock = threading.Lock()
        self.incoming_payloads: deque[bytes] = deque()
        self.delay_next_empty = threading.Event()
        self.empty_command_seen = threading.Event()
        self.release_empty_response = threading.Event()
        self.buffer = bytearray()
        self.commands: list[int] = []

    def send(self, payload: bytes) -> None:
        data = wire_frame(payload)
        offset = 0
        deadline = time.monotonic() + 1.0
        while offset < len(data):
            try:
                written = os.write(self.master_fd, data[offset:])
                offset += written
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise
                select.select([], [self.master_fd], [], 0.01)

    def queue_incoming(self, text: str, announce: bool = True,
                       timestamp: int | None = None) -> float:
        prefix = bytes.fromhex("a1b2c3d4e5f6")
        if timestamp is None:
            timestamp = int(time.time())
        payload = bytearray([16, 24, 0, 0])
        payload += prefix
        payload += bytes([0xFF, 0])
        payload += struct.pack("<I", timestamp)
        payload += text.encode("utf-8")
        with self.incoming_lock:
            self.incoming_payloads.append(bytes(payload))
        started = time.monotonic()
        if announce:
            self.send(bytes([0x83]))
        return started

    def handle_command(self, payload: bytes) -> None:
        if not payload:
            return
        code = payload[0]
        self.commands.append(code)
        if code == 1:  # CMD_APP_START -> SELF_INFO
            response = bytearray(58 + len("Latency Radio"))
            response[0] = 5
            response[1] = 1
            response[2] = 10
            response[3] = 22
            response[48:52] = struct.pack("<I", 867500)
            response[52:56] = struct.pack("<I", 250000)
            response[56] = 10
            response[57] = 5
            response[58:] = b"Latency Radio"
            self.send(bytes(response))
        elif code == 22:  # CMD_DEVICE_QUERY
            response = bytearray(80)
            response[0] = 13
            response[1] = 3
            response[2] = 64
            response[3] = 1
            response[20:20 + len("PTY MeshCore")] = b"PTY MeshCore"
            response[60:60 + len("test-1.0")] = b"test-1.0"
            self.send(bytes(response))
        elif code == 4:  # CMD_GET_CONTACTS
            self.send(bytes([2]) + struct.pack("<I", 0))
            self.send(bytes([4]) + struct.pack("<I", 0))
        elif code == 31:  # CMD_GET_CHANNEL
            index = payload[1] if len(payload) > 1 else 0
            response = bytearray(50)
            response[0] = 18
            response[1] = index
            if index == 0:
                response[2:8] = b"Public"
            self.send(bytes(response))
        elif code == 10:  # CMD_SYNC_NEXT_MESSAGE
            with self.incoming_lock:
                incoming = self.incoming_payloads.popleft() if self.incoming_payloads else None
            if incoming is None and self.delay_next_empty.is_set():
                self.delay_next_empty.clear()
                self.empty_command_seen.set()
                self.release_empty_response.wait(1.0)
                self.release_empty_response.clear()
            self.send(incoming if incoming is not None else bytes([10]))
        elif code == 2:  # CMD_SEND_TXT_MSG
            self.send_seen_at = time.monotonic()
            self.send_seen.set()
            ack = 0x78563412
            sent = bytes([6, 0]) + struct.pack("<I", ack) + struct.pack("<I", 1200)
            confirmed = bytes([0x82]) + struct.pack("<I", ack) + struct.pack("<I", 18)
            data = wire_frame(sent) + wire_frame(confirmed)
            offset = 0
            while offset < len(data):
                try:
                    offset += os.write(self.master_fd, data[offset:])
                except BlockingIOError:
                    select.select([], [self.master_fd], [], 0.01)
        else:
            self.send(bytes([0]))

    def run(self) -> None:
        os.set_blocking(self.master_fd, False)
        while not self.stop_event.is_set():
            ready, _, _ = select.select([self.master_fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(self.master_fd, 65536)
            except BlockingIOError:
                continue
            except OSError:
                return
            if not chunk:
                return
            self.buffer.extend(chunk)
            while True:
                marker = self.buffer.find(b"<")
                if marker < 0:
                    self.buffer.clear()
                    break
                if marker:
                    del self.buffer[:marker]
                if len(self.buffer) < 3:
                    break
                length = struct.unpack("<H", self.buffer[1:3])[0]
                if len(self.buffer) < 3 + length:
                    break
                payload = bytes(self.buffer[3:3 + length])
                del self.buffer[:3 + length]
                self.handle_command(payload)

    def close(self) -> None:
        self.stop_event.set()




def read_until_visible(master: int, output: bytearray, needle: str,
                       timeout: float) -> float:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.02)
        if ready:
            try:
                chunk = os.read(master, 65536)
            except OSError as exc:
                raise AssertionError(f"terminal closed before {needle!r} appeared") from exc
            if not chunk:
                raise AssertionError(f"terminal closed before {needle!r} appeared")
            output.extend(chunk)
        visible = "\n".join(render_terminal(bytes(output)))
        if needle in visible:
            return time.monotonic()
    raise AssertionError(f"terminal did not display {needle!r} within {timeout:.2f}s")

def wait_for_socket(path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise TimeoutError(f"QTC socket was not created: {path}")


def create_large_profile(binary: str, env: dict[str, str], profile: str) -> Path:
    proc = subprocess.Popen(
        [binary, "core", "--demo", "--foreground", "--profile", profile],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    socket_path = Path(env["XDG_RUNTIME_DIR"]) / "qtc" / profile / "qtc.sock"
    wait_for_socket(socket_path)
    subprocess.run([binary, "shutdown", "--profile", profile], env=env,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc.wait(timeout=5)

    db_path = Path(env["XDG_DATA_HOME"]) / "qtc-terminal" / profile / "qtc.db"
    with sqlite3.connect(db_path) as db:
        contact_id = db.execute("SELECT id FROM contacts WHERE name='Ana'").fetchone()[0]
        now = int(time.time())
        rows = []
        for index in range(1200):
            key = f"history-{index:04d}"
            rows.append((1, contact_id, 0, now - 2000 + index, 0, 3,
                         key, key, f"historical message {index}", 1, 1,
                         0, 0, 0, -1, now - 2000 + index))
        db.executemany(
            "INSERT OR IGNORE INTO messages(conversation_kind,conversation_key,direction,"
            "sender_timestamp,attempt,status,message_key,logical_key,text,part_index,part_total,"
            "ack_code,ack_deadline,snr_quarter_db,path_len,created_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            rows,
        )
        db.commit()
    return db_path


def wait_for_message(sock: socket.socket, text: str, direction: int,
                     status: int | None, timeout: float) -> float:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame_type, payload = ipc_recv(sock, max(0.01, deadline - time.monotonic()))
        if frame_type != QTC_IPC_MESSAGE or len(payload) != ctypes.sizeof(Message):
            continue
        message = Message.from_buffer_copy(payload)
        if c_string(bytes(message.text)) != text or message.direction != direction:
            continue
        if status is None or message.status == status:
            return time.monotonic()
    raise TimeoutError(f"did not receive message delta for {text!r}")


def wait_for_messages(sock: socket.socket, text: str, direction: int,
                      count: int, timeout: float) -> list[Message]:
    deadline = time.monotonic() + timeout
    found: list[Message] = []
    while time.monotonic() < deadline and len(found) < count:
        frame_type, payload = ipc_recv(sock, max(0.01, deadline - time.monotonic()))
        if frame_type != QTC_IPC_MESSAGE or len(payload) != ctypes.sizeof(Message):
            continue
        message = Message.from_buffer_copy(payload)
        if c_string(bytes(message.text)) == text and message.direction == direction:
            found.append(message)
    if len(found) != count:
        raise TimeoutError(f"received {len(found)}/{count} messages for {text!r}")
    return found


def wait_for_status(sock: socket.socket, needle: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame_type, payload = ipc_recv(sock, max(0.01, deadline - time.monotonic()))
        if frame_type != QTC_IPC_STATUS or len(payload) < 160:
            continue
        status = c_string(payload[-160:])
        if needle in status:
            return
    raise TimeoutError(f"did not receive status containing {needle!r}")


def default_binary() -> str:
    tag = "macos" if platform.system() == "Darwin" else "linux"
    return f"./build/qtc-{tag}-{platform.machine()}"


def main() -> None:
    binary = os.environ.get("QTC_BIN", default_binary())
    root = tempfile.mkdtemp(prefix="qtc-latency-")
    env = os.environ.copy()
    env["XDG_DATA_HOME"] = os.path.join(root, "data")
    env["XDG_CONFIG_HOME"] = os.path.join(root, "config")
    env["XDG_RUNTIME_DIR"] = os.path.join(root, "run")
    os.makedirs(env["XDG_RUNTIME_DIR"], exist_ok=True)
    profile = "latency"
    core: subprocess.Popen[bytes] | None = None
    client: socket.socket | None = None
    simulator: RadioSimulator | None = None
    master = slave = -1
    tui_master = tui_slave = -1
    tui: subprocess.Popen[bytes] | None = None
    try:
        create_large_profile(binary, env, profile)
        master, slave = pty.openpty()
        slave_path = os.ttyname(slave)
        simulator = RadioSimulator(master)
        simulator.start()
        core = subprocess.Popen(
            [binary, "core", "--foreground", "--profile", profile,
             "--device", slave_path],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        socket_path = Path(env["XDG_RUNTIME_DIR"]) / "qtc" / profile / "qtc.sock"
        wait_for_socket(socket_path)
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.connect(str(socket_path))
        ipc_send(client, QTC_IPC_HELLO,
                 struct.pack("<I32s16s", 2, b"latency-test", b"1.0.0"))
        while True:
            frame_type, _ = ipc_recv(client, 5.0)
            if frame_type == QTC_IPC_STATE_END:
                break

        if simulator.commands[:2] != [1, 22]:
            raise AssertionError(
                f"MeshCore startup was not APP_START -> DEVICE_QUERY: {simulator.commands[:6]!r}"
            )
        if any(code in (2, 4, 10) for code in simulator.commands[:2]):
            raise AssertionError("interactive/inbox work ran before MeshCore startup completed")

        direct_text = "instant serial send"
        payload = bytearray(65 + 768)
        payload[:12] = b"a1b2c3d4e5f6"
        encoded = direct_text.encode("utf-8")
        payload[65:65 + len(encoded)] = encoded
        started = time.monotonic()
        ipc_send(client, QTC_IPC_SEND_DIRECT, bytes(payload))

        # Deliberately do not read UI updates yet. A slow terminal must never
        # block the core before it writes the command to USB serial.
        if not simulator.send_seen.wait(0.40):
            raise AssertionError("direct message did not reach serial within 400 ms")
        serial_latency = simulator.send_seen_at - started
        if serial_latency > 0.25 * TIMING_SCALE:
            raise AssertionError(f"serial send latency was {serial_latency * 1000:.1f} ms")

        delivered_at = wait_for_message(client, direct_text, QTC_MSG_OUTGOING,
                                        QTC_MSG_DELIVERED, 1.0)
        confirmation_latency = delivered_at - started
        if confirmation_latency > 0.50 * TIMING_SCALE:
            raise AssertionError(
                f"local delivered confirmation took {confirmation_latency * 1000:.1f} ms"
            )

        incoming_text = "instant incoming message"
        incoming_started = simulator.queue_incoming(incoming_text)
        incoming_at = wait_for_message(client, incoming_text, QTC_MSG_INCOMING,
                                       None, 1.0)
        receive_latency = incoming_at - incoming_started
        if receive_latency > 0.50 * TIMING_SCALE:
            raise AssertionError(f"incoming delivery took {receive_latency * 1000:.1f} ms")

        # A companion push should be the normal path, but a missed push must not
        # make the inbox unreliable. The fast fallback poll should recover it.
        fallback_text = "fallback-polled incoming message"
        fallback_started = simulator.queue_incoming(fallback_text, announce=False)
        fallback_at = wait_for_message(client, fallback_text, QTC_MSG_INCOMING,
                                       None, 1.0)
        fallback_latency = fallback_at - fallback_started
        if fallback_latency > 0.60 * TIMING_SCALE:
            raise AssertionError(
                f"fallback inbox delivery took {fallback_latency * 1000:.1f} ms"
            )

        # A PUSH_CODE_MSG_WAITING can arrive while an older empty inbox query
        # is still awaiting PACKET_NO_MORE_MSGS.  The old boolean drain flag
        # let that stale empty response cancel the newer push.  Reproduce the
        # race explicitly and require the message to survive it.
        simulator.delay_next_empty.set()
        if not simulator.empty_command_seen.wait(1.0):
            raise AssertionError("core did not issue its fallback inbox poll")
        raced_text = "message arriving during stale empty response"
        raced_started = simulator.queue_incoming(raced_text, announce=True)
        simulator.release_empty_response.set()
        raced_at = wait_for_message(client, raced_text, QTC_MSG_INCOMING,
                                    None, 1.0)
        if raced_at - raced_started > 0.60 * TIMING_SCALE:
            raise AssertionError("message-waiting/empty-response race was too slow")

        # One waiting notification can represent many queued radio messages.
        # Drain the complete queue without waiting for another notification.
        burst_count = 24
        burst_started = time.monotonic()
        for index in range(burst_count):
            simulator.queue_incoming(f"burst message {index:02d}", announce=False)
        simulator.send(bytes([0x83]))
        remaining = {f"burst message {index:02d}" for index in range(burst_count)}
        deadline = time.monotonic() + 2.0 * TIMING_SCALE
        while remaining and time.monotonic() < deadline:
            frame_type, payload = ipc_recv(client, max(0.01, deadline - time.monotonic()))
            if frame_type != QTC_IPC_MESSAGE or len(payload) != ctypes.sizeof(Message):
                continue
            message = Message.from_buffer_copy(payload)
            if message.direction == QTC_MSG_INCOMING:
                remaining.discard(c_string(bytes(message.text)))
        if remaining:
            raise AssertionError(f"inbox burst lost messages: {sorted(remaining)!r}")
        if time.monotonic() - burst_started > 2.0 * TIMING_SCALE:
            raise AssertionError("inbox burst drain was too slow")

        # The wire protocol has only second-resolution timestamps. Two equal
        # messages sent during the same second are still two queue entries and
        # must not collapse under the database uniqueness key.
        identical_text = "same text twice in one second"
        identical_timestamp = int(time.time())
        simulator.queue_incoming(identical_text, announce=False,
                                 timestamp=identical_timestamp)
        simulator.queue_incoming(identical_text, announce=False,
                                 timestamp=identical_timestamp)
        simulator.send(bytes([0x83]))
        identical = wait_for_messages(client, identical_text, QTC_MSG_INCOMING,
                                      2, 1.0)
        if c_string(bytes(identical[0].message_key)) == c_string(bytes(identical[1].message_key)):
            raise AssertionError("same-second identical messages shared a database key")

        # Advert actions need positive completion feedback, not only a keypress.
        advert = DeviceAction()
        advert.flag = False
        ipc_send(client, QTC_IPC_DEVICE_ADVERTISE, bytes(advert))
        wait_for_status(client, "0-hop advertisement sent", 1.0)
        advert.flag = True
        ipc_send(client, QTC_IPC_DEVICE_ADVERTISE, bytes(advert))
        wait_for_status(client, "Flood advertisement sent", 1.0)

        # The composer is a live view, not a blocking modal. While an unsent
        # draft is present, an incoming radio message must appear immediately
        # and the draft/cursor state must survive the redraw.
        tui_master, tui_slave = pty.openpty()
        fcntl.ioctl(tui_slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, 0, 0))
        tui = subprocess.Popen(
            [binary, "--profile", profile],
            stdin=tui_slave,
            stdout=tui_slave,
            stderr=tui_slave,
            env=env,
            close_fds=True,
        )
        os.close(tui_slave)
        tui_slave = -1
        terminal_output = bytearray(read_available(tui_master, 1.2))
        os.write(tui_master, b"\x1b[B\x1b[B\r")  # select Ana, open compose
        terminal_output.extend(read_available(tui_master, 0.3))
        draft = "draft remains while receiving"
        os.write(tui_master, draft.encode("utf-8"))
        terminal_output.extend(read_available(tui_master, 0.2))

        compose_incoming = "message received during compose"
        compose_started = simulator.queue_incoming(compose_incoming)
        compose_visible_at = read_until_visible(tui_master, terminal_output,
                                                compose_incoming, 0.75)
        compose_latency = compose_visible_at - compose_started
        screen = render_terminal(bytes(terminal_output))
        visible = "\n".join(screen)
        if compose_incoming not in visible:
            raise AssertionError("incoming message was hidden until compose mode ended")
        if draft not in visible:
            raise AssertionError("incoming redraw erased the unsent compose draft")
        if "NEW" not in screen[ROWS - 2]:
            raise AssertionError("compose mode did not show the incoming-message banner")
        if compose_latency > 1.0 * TIMING_SCALE:
            raise AssertionError(
                f"compose-mode display took {compose_latency * 1000:.1f} ms"
            )
        os.write(tui_master, b" + still typing")
        read_until_visible(tui_master, terminal_output,
                           draft + " + still typing", 0.50)
        visible = "\n".join(render_terminal(bytes(terminal_output)))
        if draft + " + still typing" not in visible:
            raise AssertionError("composer stopped accepting input after async receive")
        os.write(tui_master, b"\x03")
        tui.wait(timeout=3)
        os.close(tui_master)
        tui_master = -1

        print(
            "serial latency test passed: "
            f"tx={serial_latency * 1000:.1f}ms, "
            f"confirm={confirmation_latency * 1000:.1f}ms, "
            f"rx={receive_latency * 1000:.1f}ms, "
            f"fallback-rx={fallback_latency * 1000:.1f}ms, "
            f"compose-rx={compose_latency * 1000:.1f}ms"
        )
    finally:
        if tui is not None and tui.poll() is None:
            try:
                os.write(tui_master, b"\x03")
                tui.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                tui.kill()
        if client is not None:
            client.close()
        subprocess.run([binary, "shutdown", "--profile", profile], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False, timeout=3)
        if core is not None:
            try:
                core.wait(timeout=3)
            except subprocess.TimeoutExpired:
                core.kill()
        if simulator is not None:
            simulator.close()
            simulator.join(timeout=1)
        for fd in (master, slave, tui_master, tui_slave):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
