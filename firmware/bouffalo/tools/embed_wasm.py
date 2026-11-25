#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Embed an existing PC-compatible Wasm module in the Orion app catalog."""

import argparse
import lzma
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wasm", type=Path)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()
    binary = args.wasm.read_bytes()
    if binary[:8] != b"\0asm\x01\0\0\0":
        parser.error(f"{args.wasm} is not a Wasm v1 module")
    name = args.wasm.stem.replace("-", "_")
    if not name.isidentifier():
        parser.error(f"{name!r} is not a valid catalog symbol")
    packed = lzma.compress(binary, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32)
    rows = [", ".join(f"0x{byte:02x}" for byte in packed[offset : offset + 16])
            for offset in range(0, len(packed), 16)]
    args.header.write_text(
        "// Generated Wasm catalog entry.\n#pragma once\n\n#include <cstdint>\n\n"
        f"static constexpr uint32_t {name}_wasm_size = {len(binary)};\n"
        f"static constexpr uint8_t {name}[] = {{\n    "
        + ",\n    ".join(rows)
        + "\n};\n"
    )


if __name__ == "__main__":
    main()
