#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""List, import or remove Orion Bundle v1 applications over the UART console."""

import argparse
from pathlib import Path
import os
import re
import sys
import time

from pack_orion_apps import bundle_id, compress_bundle

CHUNK_BYTES = 44
APP_LINE = re.compile(rb"MPX APP ([\w.-]+) ([\d.]+) (\w+)")
STORE_LINE = re.compile(rb"MPX STORE ([a-z_]+) ([a-z_]+)(?: ([^\r\n]*?))?(?=MPX |\r?\n|$)")


class UartLines:
    def __init__(self, device):
        self.device = device
        self.buffer = bytearray()

    def write(self, data):
        return self.device.write(data)

    def read_until(self):
        while (end := self.buffer.find(b"\n")) < 0:
            chunk = self.device.read(min(self.device.in_waiting, 8192) or 1)
            if not chunk:
                return b""
            self.buffer.extend(chunk)
            if len(self.buffer) > 8192 and b"\n" not in self.buffer:
                del self.buffer[:-4096]
        line = bytes(self.buffer[:end + 1])
        del self.buffer[:end + 1]
        return line


class Console:
    def __init__(self, uart):
        self.uart = uart
        self.partial = bytearray()

    def command(self, command):
        self.uart.write(f"\rmpx bundle {command}\r".encode("ascii"))

    def reply(self, operation, timeout=5, offset=None):
        deadline = time.monotonic() + timeout
        records = {}
        while time.monotonic() < deadline:
            self.partial.extend(self.uart.read_until())
            if not self.partial.endswith(b"\n"):
                if len(self.partial) > 4096:
                    self.partial.clear()
                continue
            line = bytes(self.partial)
            self.partial.clear()
            if operation == "list" and (app := APP_LINE.search(line)):
                records[app[1]] = b" ".join(app.groups()).decode("ascii")
            for match in STORE_LINE.finditer(line):
                if match[1].decode("ascii") != operation:
                    continue
                status = match[2].decode("ascii")
                detail = (match[3] or b"").decode("ascii")
                if offset is not None and detail.strip() != str(offset):
                    continue
                if status == "pending" and operation == "apply":
                    continue
                if status != "ok":
                    raise ValueError(f"Orion {operation}: {status}")
                if operation == "list":
                    fields = detail.split()
                    if len(fields) != 2 or not all(field.isdecimal() for field in fields) or int(fields[0]) != len(records):
                        raise TimeoutError("Incomplete Orion application list")
                    for record in records.values():
                        print(record)
                return detail
        raise TimeoutError(f"No Orion response for {operation}")

    def wait_ready(self, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.uart.write(b"\rmpx status\r")
            attempt = min(deadline, time.monotonic() + 2)
            while time.monotonic() < attempt:
                if b"Orion LCD frames=" in self.uart.read_until():
                    return
        raise TimeoutError("No Orion console response")

    def exchange(self, command, timeout=5):
        self.command(command)
        return self.reply(command.split()[0], timeout)

    def upload(self, raw, packed):
        self.exchange(f"begin {len(packed)} {len(raw)}")
        try:
            for pos in range(0, len(packed), CHUNK_BYTES):
                command = f"chunk {pos} {packed[pos:pos + CHUNK_BYTES].hex()}"
                for attempt in range(3):
                    self.command(command)
                    try:
                        self.reply("chunk", timeout=1, offset=pos)
                    except TimeoutError:
                        if attempt == 2:
                            raise
                    else:
                        break
            self.exchange("ready", timeout=30)
        except (ValueError, OSError, KeyboardInterrupt):
            try:
                self.exchange("cancel", timeout=2)
            except (ValueError, OSError):
                pass
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Orion UART console at 2 Mbps")
    actions = parser.add_subparsers(dest="operation", required=True)
    actions.add_parser("list")
    upload = actions.add_parser("import", help="Stage an install or upgrade")
    upload.add_argument("bundle", type=Path)
    upload.add_argument("--apply", action="store_true", help="Confirm without using the device screen")
    remove = actions.add_parser("remove", help="Stage an uninstall")
    remove.add_argument("app_id")
    remove.add_argument("--apply", action="store_true", help="Confirm without using the device screen")
    actions.add_parser("apply", help="Confirm a staged operation")
    actions.add_parser("cancel", help="Cancel a staged operation")
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.exit(1, "Install pyserial from requirements-dev.txt.\n")

    try:
        if args.operation == "import":
            raw = args.bundle.read_bytes()
            name = bundle_id(raw)
            packed = compress_bundle(raw)
        if args.operation == "remove" and not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9_.-]{0,63}", args.app_id):
            raise ValueError("Invalid AppId")
        options = {"exclusive": True} if os.name != "nt" else {}
        link = serial.Serial(port=None, baudrate=2_000_000, timeout=0.25, write_timeout=3, **options)
        link.dtr = True
        link.rts = False
        link.port = args.port
        with link as uart:
            if os.name == "posix":
                import termios

                settings = termios.tcgetattr(uart.fileno())
                settings[2] &= ~termios.HUPCL
                termios.tcsetattr(uart.fileno(), termios.TCSANOW, settings)
            uart.reset_input_buffer()
            console = Console(UartLines(uart))
            console.wait_ready()
            if args.operation == "list":
                for attempt in range(3):
                    try:
                        count, free = console.exchange("list", timeout=7).split()
                    except TimeoutError:
                        if attempt == 2:
                            raise
                    else:
                        break
                print(f"{count} installed, {int(free) // 1024} KiB free")
            elif args.operation == "import":
                print(f"Importing {name} ({len(packed)} bytes)", flush=True)
                console.upload(raw, packed)
            elif args.operation == "remove":
                console.exchange(f"remove {args.app_id}")
            elif args.operation == "cancel":
                console.exchange("cancel")

            if args.operation == "apply" or getattr(args, "apply", False):
                console.exchange("apply", timeout=30)
                print("Application store updated")
            elif args.operation in {"import", "remove"}:
                print("Staged. Confirm in Orion 应用管理.")
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
