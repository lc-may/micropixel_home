#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the Orion factory LittleFS image from installable Bundle v1 packages."""

import argparse
import lzma
import os
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import tomllib

PARTITION = "mp_apps"
MAX_APPS = 15
MAX_PACKED_BYTES = 768 * 1024
MAX_BUNDLE_BYTES = 2 * 1024 * 1024
FILE_MAGIC = b"MPXZ"
HEADER = struct.Struct("<4sI")


def compress_bundle(data: bytes) -> bytes:
    if not 0 < len(data) <= MAX_BUNDLE_BYTES:
        raise ValueError(f"Bundle exceeds {MAX_BUNDLE_BYTES} bytes")
    if len(data) % (64 * 1024):
        raise ValueError("Bundle size must be aligned to a 64 KiB extent")
    packed = lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32)
    if len(packed) > MAX_PACKED_BYTES:
        raise ValueError(f"Compressed Bundle exceeds {MAX_PACKED_BYTES} bytes")
    return packed


def bundle_id(data: bytes) -> str:
    if len(data) < 128 or data[:8] != b"MPXBNDL\0":
        raise ValueError("Expected a MicroPixel Bundle v1")
    length, = struct.unpack_from("<I", data, 88)
    name = data[24:24 + length].decode("ascii")
    if not 0 < length <= 64 or not all(c.isalnum() or c in ".-_" for c in name) or name.startswith("."):
        raise ValueError(f"Invalid Bundle AppId {name!r}")
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("partitions", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("bundles", nargs="+", type=Path)
    args = parser.parse_args()
    entries = tomllib.loads(args.partitions.read_text())["pt_entry"]
    capacity = next(entry["size0"] for entry in entries if entry["name"] == PARTITION)
    if len(args.bundles) > MAX_APPS:
        parser.error(f"Orion supports at most {MAX_APPS} installed applications")
    with TemporaryDirectory(prefix="orion-factory-") as directory:
        temporary = Path(directory)
        encoder = temporary / "app-store-image"
        littlefs = args.sdk / "components/fs/littlefs/littlefs"
        subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-O2", "-I", str(littlefs),
                        str(Path(__file__).with_name("orion_app_store_image.c")),
                        str(littlefs / "lfs.c"), str(littlefs / "lfs_util.c"), "-o", str(encoder)], check=True)
        total = 0
        inputs = []
        for path in args.bundles:
            data = path.read_bytes()
            name = bundle_id(data)
            packed = compress_bundle(data)
            inputs.append(temporary / name)
            with inputs[-1].open("xb") as file:
                file.write(HEADER.pack(FILE_MAGIC, len(data)))
                file.write(packed)
            total += len(packed) + HEADER.size
        image = args.image.with_suffix(".tmp")
        try:
            subprocess.run([str(encoder), str(image), str(capacity), *map(str, inputs)], check=True)
            image.replace(args.image)
        finally:
            image.unlink(missing_ok=True)
    if args.image.stat().st_size != capacity:
        parser.error("Factory App Store image size does not match the partition")
    print(f"Orion factory App Store: {len(args.bundles)} apps, {total}/{capacity} payload bytes")


if __name__ == "__main__":
    main()
