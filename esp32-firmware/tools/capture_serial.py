#!/usr/bin/env python3
"""Capture ESP32 diagnostics or run a shared interactive Serial session.

Run on the host if the agent's sandbox does not expose the serial device:
    python3 tools/capture_serial.py
    python3 tools/capture_serial.py --interactive
Send a command to an already running interactive session from another shell:
    python3 tools/capture_serial.py --send s
Status is requested automatically; other Serial commands require explicit input.
The helper never uploads firmware.
"""
import argparse
from contextlib import ExitStack, contextmanager
import math
import os
from pathlib import Path
import select
import stat
import sys
import termios
import time


ROOT = Path(__file__).resolve().parents[1]
MAX_COMMAND_BYTES = 1024  # Less than Linux PIPE_BUF: concurrent sends stay atomic.


def validate_command(command):
    command = command.strip()
    if len(command.encode("utf-8")) + 1 > MAX_COMMAND_BYTES:
        raise ValueError("Command is too long")
    if any(ord(char) < 32 or ord(char) == 127 for char in command):
        raise ValueError("Commands must be one line without control characters")
    if command in ("/quit", "/help") or command.startswith("/note "):
        return command
    if len(command) != 1 or not command.isascii() or not command.isprintable():
        raise ValueError("Send one Serial character (e.g. s), /note TEXT, /help, or /quit")
    return command


def send_command(path, command):
    # O_NONBLOCK avoids hanging if a capture stopped and left a stale FIFO.
    fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        if not stat.S_ISFIFO(os.fstat(fd).st_mode):
            raise OSError(f"{path} is not a control FIFO")
        payload = (command + "\n").encode("utf-8")
        if os.write(fd, payload) != len(payload):
            raise OSError("Incomplete control command")
    finally:
        os.close(fd)
    print(f"Queued for the active Serial session: {command}", flush=True)


@contextmanager
def serial_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        settings = termios.tcgetattr(fd)
        settings[0] = 0
        settings[1] = 0
        settings[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        settings[3] = 0
        settings[4] = settings[5] = termios.B115200
        settings[6][termios.VMIN] = 0
        settings[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        yield fd
    finally:
        # Leave the device at 115200/raw when closing. Restoring a default TTY
        # with ECHO enabled can send the ESP's own boot output back as commands
        # while it is unattended or during the next open/configure interval.
        # Serial commands must come only from explicit helper writes.
        os.close(fd)


@contextmanager
def control_pipe(path):
    # Refuse existing paths: never replace another session or a regular file.
    os.mkfifo(path, 0o600)
    created = path.stat()
    fd = None
    try:
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW)
        yield fd
    finally:
        if fd is not None:
            os.close(fd)
        try:
            current = path.lstat()
            if (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                path.unlink()
        except FileNotFoundError:
            pass


def interactive_help():
    print("Type one Serial character, then Enter: s=status, ?=help, c=resume RC, r=pause RC.", flush=True)
    print("/note TEXT logs an observation; /quit stops; /help shows this help.", flush=True)
    print("p is the firmware's profile-clear command. Ctrl+C stops; tmux Ctrl+B then D detaches.", flush=True)


def capture(args, fd, log, control_fd=None):
    duration = f"{args.seconds:g}s" if args.seconds else "until stopped"
    print(f"Capturing {args.port} at 115200 baud {duration} -> {args.output}", flush=True)
    print("Close Arduino Serial Monitor first. Press EN/reset to capture startup; wake the remote.", flush=True)
    if args.interactive:
        print(f"Shared command FIFO: {args.control_path}", flush=True)
        interactive_help()
    inputs = {}
    if control_fd is not None:
        inputs[control_fd] = ("shared", bytearray())
        inputs[sys.stdin.fileno()] = ("terminal", bytearray())
    outgoing = bytearray()

    def note(message):
        line = f"# {time.strftime('%Y-%m-%d %H:%M:%S')} {message}\n".encode("utf-8")
        log.write(line)
        log.flush()
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()

    def handle_command(line, origin):
        if not line.strip():
            return True
        try:
            command = validate_command(line.decode("utf-8"))
        except (ValueError, UnicodeError) as error:
            note(f"REJECT ({origin}): {error}")
            return True
        if command == "/quit":
            note(f"STOP requested from {origin}")
            return False
        if command == "/help":
            interactive_help()
        elif command.startswith("/note "):
            note(f"NOTE ({origin}): {command[6:]}")
        else:
            if len(outgoing) >= 4096:
                raise OSError("Serial command output queue full")
            outgoing.extend((command + "\n").encode("ascii"))
            note(f"SEND ({origin}): {command}")
        return True

    note("Interactive session started" if args.interactive else "Capture started")
    started = time.monotonic()
    deadline = started + args.seconds if args.seconds else float("inf")
    next_status = started + 1
    while time.monotonic() < deadline:
        now = time.monotonic()
        if args.status_interval and now >= next_status:
            handle_command(b"s", "auto")
            next_status = now + args.status_interval
        readable, writable, _ = select.select(
            [fd, *inputs], [fd] if outgoing else [], [], min(0.25, max(0, deadline - now)))
        if fd in writable:
            try:
                written = os.write(fd, outgoing)
                del outgoing[:written]
            except BlockingIOError:
                pass
        for ready in readable:
            try:
                data = os.read(ready, 8192)
            except BlockingIOError:
                continue
            if ready == fd:
                if not data:
                    raise OSError("Serial device closed or disconnected")
                log.write(data)
                log.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                continue
            origin, buffer = inputs[ready]
            if not data:
                if buffer and not handle_command(bytes(buffer), origin):
                    return
                del inputs[ready]
                continue
            buffer.extend(data)
            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer[:] = rest
                if not handle_command(line, origin):
                    return
            if len(buffer) > MAX_COMMAND_BYTES:
                raise OSError(f"Unterminated command too long ({origin})")
    note("Capture duration reached")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--interactive", action="store_true", help="stay running; accept terminal and shared commands")
    mode.add_argument("--send", metavar="COMMAND", help="send to an active interactive session and exit")
    parser.add_argument("--seconds", type=float, help="duration; default 45, or unlimited in interactive mode")
    parser.add_argument("--status-interval", type=float, default=10, help="seconds between automatic s commands; 0 disables")
    parser.add_argument("--output", type=Path, default=ROOT / "serial-diagnostic.log")
    parser.add_argument("--append", action="store_true", help="append to the log instead of replacing it")
    parser.add_argument("--control-path", type=Path, default=ROOT / "serial-control.fifo")
    args = parser.parse_args()
    if args.send is not None:
        try:
            command = validate_command(args.send)
            send_command(args.control_path, command)
        except (ValueError, OSError) as error:
            print(f"Cannot send command: {error}. Start --interactive on the host first.", file=sys.stderr)
            return 1
        return 0
    if args.seconds is None:
        args.seconds = 0 if args.interactive else 45
    if not math.isfinite(args.seconds) or args.seconds < 0 or (not args.interactive and args.seconds == 0):
        parser.error("--seconds must be positive (0 means unlimited with --interactive)")
    if not math.isfinite(args.status_interval) or args.status_interval < 0:
        parser.error("--status-interval must be nonnegative")
    try:
        with ExitStack() as stack:
            # Reserve the shared endpoint before opening the port or log, so a
            # second session cannot disturb a running session's settings/log.
            control_fd = stack.enter_context(control_pipe(args.control_path)) if args.interactive else None
            fd = stack.enter_context(serial_port(args.port))
            log = stack.enter_context(args.output.open("ab" if args.append else "wb"))
            capture(args, fd, log, control_fd)
    except KeyboardInterrupt:
        print("\nCapture stopped.", flush=True)
    except (OSError, termios.error) as error:
        print(f"Capture failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
