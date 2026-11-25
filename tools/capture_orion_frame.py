#!/usr/bin/env python3
"""Read a 480×480 Orion LCD frame from UART and save a verified PNG."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import re
import select
import struct
import termios
import time
import tty
import zlib

from PIL import Image

WIDTH = HEIGHT = 480
CHUNK_BYTES = 256
HEADER = re.compile(rb"MPXFB RGB565LE (\d+) (\d+) ([0-9a-f]{8})")
CHUNK = re.compile(rb"MPXFB ([0-9a-f]{4}) ([0-9a-f]{512})")


def uart_lines(fd: int, deadline: float):
    pending = b""
    while (remaining := deadline - time.monotonic()) > 0:
        if not select.select([fd], [], [], remaining)[0]:
            break
        block = os.read(fd, 8192)
        if not block:
            break
        *lines, pending = (pending + block).split(b"\n")
        for line in lines:
            yield line.rstrip(b"\r")
        if len(pending) > 8192:
            pending = b""


@contextmanager
def open_console(port: Path):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = None
    try:
        fcntl.ioctl(fd, termios.TIOCEXCL)
        previous = termios.tcgetattr(fd)
        tty.setraw(fd)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B2000000
        settings[2] = (settings[2] | termios.CLOCAL | termios.CREAD) & ~(termios.CRTSCTS | termios.HUPCL)
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        # The CH347 asserts RTS on open. Release the board's reset line before
        # sending a command, and keep modem outputs stable on close.
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", termios.TIOCM_RTS))
        termios.tcflush(fd, termios.TCIFLUSH)
        yield fd
    finally:
        if previous is not None:
            previous[2] &= ~termios.HUPCL
            termios.tcsetattr(fd, termios.TCSANOW, previous)
        os.close(fd)


def wait_ready(fd: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        os.write(fd, b"mpx status\r")
        if any(b"Orion LCD frames=" in line
               for line in uart_lines(fd, min(deadline, time.monotonic() + 0.7))):
            return
    raise ValueError("no Orion console response")


def receive(fd: int, timeout: float) -> bytes:
    os.write(fd, b"mpx screenshot\r")
    lines = uart_lines(fd, time.monotonic() + timeout)
    chunks = WIDTH * HEIGHT * 2 // CHUNK_BYTES
    for line in lines:
        header = HEADER.fullmatch(line)
        if header:
            break
        if b"MPXFB capture failed" in line:
            raise ValueError("Orion reported that the display frame could not be captured")
    else:
        raise ValueError("no Orion frame header received")
    if (int(header[1]), int(header[2])) != (WIDTH, HEIGHT):
        raise ValueError("unexpected Orion display dimensions")
    result = bytearray()
    for line in lines:
        chunk = CHUNK.fullmatch(line)
        if not chunk:
            continue
        index = len(result) // CHUNK_BYTES
        if int(chunk[1], 16) != index:
            raise ValueError(f"damaged UART frame at chunk {index}")
        result.extend(bytes.fromhex(chunk[2].decode("ascii")))
        if index + 1 == chunks:
            if zlib.crc32(result) != int(header[3], 16):
                raise ValueError("Orion frame CRC did not match")
            return bytes(result)
    raise ValueError(f"incomplete Orion framebuffer: received {len(result)} bytes")


def rgb_image(data: bytes) -> Image.Image:
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack("<H", data):
        red, green, blue = pixel >> 11, (pixel >> 5) & 63, pixel & 31
        rgb.extend(((red * 255 + 15) // 31, (green * 255 + 31) // 63, (blue * 255 + 15) // 31))
    return Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(rgb))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=Path, required=True, help="Orion UART console at 2 Mbps")
    parser.add_argument("--timeout", default=20.0, type=float)
    parser.add_argument("output", type=Path, help="output PNG")
    args = parser.parse_args()
    try:
        with open_console(args.port) as fd:
            wait_ready(fd, min(args.timeout, 8))
            data = receive(fd, args.timeout)
        rgb_image(data).save(args.output, format="PNG")
    except (ValueError, OSError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{WIDTH}×{HEIGHT} Orion framebuffer verified by CRC32: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
