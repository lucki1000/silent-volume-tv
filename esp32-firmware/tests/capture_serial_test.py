#!/usr/bin/env python3
"""Exercise the real serial helper through a pseudo-terminal and shared FIFO."""
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time

SCRIPT = Path(__file__).resolve().parents[1] / "tools/capture_serial.py"


def wait_for(check, message, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.01)
    raise AssertionError(message)


def receive(fd, expected):
    data = bytearray()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.05)[0]:
            data.extend(os.read(fd, 8192))
            if expected in data:
                return bytes(data)
    raise AssertionError(f"Serial output missing {expected!r}; got {bytes(data)!r}")


def cli(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          capture_output=True, timeout=3)


with tempfile.TemporaryDirectory(prefix="esp32-serial-test-") as directory:
    directory = Path(directory)
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    assert original[3] & termios.ECHO, "Fixture must start with unsafe default echo"
    port = os.ttyname(slave)
    log = directory / "serial.log"
    fifo = directory / "control.fifo"
    active = None
    try:
        # Default interactive mode runs indefinitely, even with no Serial data.
        active = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--interactive", "--port", port,
             "--output", str(log), "--control-path", str(fifo), "--status-interval", "0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wait_for(lambda: log.exists() and b"Interactive session started" in log.read_bytes(),
                 "Interactive capture did not start")
        assert fifo.exists() and (fifo.stat().st_mode & 0o777) == 0o600
        os.write(master, b"Remote ready=yes\r\nRX id=3 [2]: 42 00\r\n")
        wait_for(lambda: b"RX id=3 [2]: 42 00" in log.read_bytes(), "Serial RX was not logged")

        # Agent command crosses the shared workspace pipe without opening serial.
        sent = cli("--send", "s", "--control-path", fifo)
        assert sent.returncode == 0, sent.stderr
        assert receive(master, b"s\n") == b"s\n"
        wait_for(lambda: b"SEND (shared): s" in log.read_bytes(), "Shared send was not logged")

        # Terminal input and annotated observations are handled independently.
        active.stdin.write(b"c\n/note TV shows connected\n")
        active.stdin.flush()
        assert receive(master, b"c\n") == b"c\n"
        wait_for(lambda: b"NOTE (terminal): TV shows connected" in log.read_bytes(),
                 "Terminal observation was not logged")
        assert cli("--send", "/note Pressed Up", "--control-path", fifo).returncode == 0
        wait_for(lambda: b"NOTE (shared): Pressed Up" in log.read_bytes(), "Shared note missing")
        assert not select.select([master], [], [], 0.1)[0], "Notes leaked into firmware commands"

        # Words are rejected: 'up' must never accidentally execute firmware p.
        assert cli("--send", "up", "--control-path", fifo).returncode != 0
        assert cli("--send", "s\np", "--control-path", fifo).returncode != 0
        active.stdin.write(b"up\n")
        active.stdin.flush()
        wait_for(lambda: b"REJECT (terminal)" in log.read_bytes(), "Invalid terminal command accepted")
        assert not select.select([master], [], [], 0.1)[0], "Invalid command reached ESP32"

        # A second helper must not replace the endpoint, truncate the log or
        # change the serial termios settings while the first is running.
        before = log.read_bytes()
        settings = termios.tcgetattr(slave)
        second = cli("--interactive", "--port", port, "--output", log, "--control-path", fifo)
        assert second.returncode != 0
        assert log.read_bytes() == before and termios.tcgetattr(slave) == settings
        assert active.poll() is None

        assert cli("--send", "/quit", "--control-path", fifo).returncode == 0
        stdout, stderr = active.communicate(timeout=3)
        assert active.returncode == 0, stderr
        assert b"RX id=3 [2]: 42 00" in stdout
        assert not fifo.exists() and termios.tcgetattr(slave) == settings
        # Boot output while no helper runs must not echo back as ESP commands.
        os.write(master, b"p=clear learned bridge profile\r\n")
        assert not select.select([master], [], [], 0.1)[0], "Closed capture restored unsafe echo"
        assert cli("--send", "s", "--control-path", fifo).returncode != 0
        active = None

        # Finite capture remains compatible; status polling and append work.
        prior = log.read_bytes()
        active = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--port", port, "--seconds", "1.3",
             "--status-interval", "10", "--output", str(log), "--append"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert receive(master, b"s\n") == b"s\n"
        _, stderr = active.communicate(timeout=3)
        assert active.returncode == 0, stderr
        assert log.read_bytes().startswith(prior) and not fifo.exists()
        assert termios.tcgetattr(slave) == settings
        active = None

        # Interrupt cleanup leaves safe raw mode and removes the shared endpoint.
        log.unlink()
        active = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--interactive", "--port", port,
             "--output", str(log), "--control-path", str(fifo), "--status-interval", "0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wait_for(lambda: log.exists() and b"Interactive session started" in log.read_bytes()
                 and fifo.exists(), "Interrupt fixture did not start")
        active.send_signal(signal.SIGINT)
        _, stderr = active.communicate(timeout=3)
        assert active.returncode == 0, stderr
        assert not fifo.exists() and termios.tcgetattr(slave) == settings
        active = None

        # Refuse ordinary files/symlinks and fail quickly on stale endpoints.
        fifo.write_text("keep me")
        assert cli("--send", "s", "--control-path", fifo).returncode != 0
        assert fifo.read_text() == "keep me"
        fifo.unlink()
        target = directory / "target"
        target.write_text("keep me too")
        fifo.symlink_to(target)
        assert cli("--send", "s", "--control-path", fifo).returncode != 0
        assert target.read_text() == "keep me too"
        fifo.unlink()
        os.mkfifo(fifo)
        assert cli("--send", "s", "--control-path", fifo).returncode != 0
        fifo.unlink()
        assert cli("--interactive", "--port", directory / "missing",
                   "--control-path", fifo, "--output", log).returncode != 0
        assert not fifo.exists()
        for value in ("nan", "inf", "-1"):
            assert cli("--seconds", value).returncode != 0
            assert cli("--status-interval", value).returncode != 0
    finally:
        if active is not None and active.poll() is None:
            active.kill()
            active.communicate(timeout=3)
        os.close(master)
        os.close(slave)

print("Interactive Serial tests passed: RX log, terminal/FIFO commands, notes, polling, cleanup and endpoint ownership.")
