#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack the Orion AOT catalog for the apps partition and bind it to the Host."""

import argparse
import hashlib
import lzma
from pathlib import Path
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("partitions", type=Path)
    parser.add_argument("header", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    entries = tomllib.loads(args.partitions.read_text())["pt_entry"]
    capacity = next(entry["size0"] for entry in entries if entry["name"] == "apps")

    data = bytearray()
    modules = []
    symbols = set()
    for path in args.inputs:
        name = path.name.removesuffix(".aot.xz")
        symbol = name.replace("-", "_")
        if not symbol.isascii() or not symbol.isidentifier() or symbol in symbols:
            parser.error(f"Invalid or duplicate Orion app symbol {symbol!r}")
        symbols.add(symbol)
        packed = path.read_bytes()
        module = lzma.decompress(packed)
        if module[:8] != b"\0aot\x06\0\0\0":
            parser.error(f"{path} is not a WAMR AOT v6 module")
        modules.append((name, len(data), len(packed), len(module)))
        data.extend(packed)

    if len(data) > capacity:
        parser.error(f"Orion AOT catalog takes {len(data)} bytes and apps holds {capacity}")
    digest = ", ".join(f"0x{byte:02x}" for byte in hashlib.sha256(data).digest())
    images = "\n".join(f'    {{"{name}", {offset}, {packed}, {unpacked}}},'
                       for name, offset, packed, unpacked in modules)
    args.image.write_bytes(data)
    args.header.write_text(
        "// Generated from the Orion AOT apps image.\n#pragma once\n#include <cstdint>\n\n"
        f"inline constexpr uint32_t kOrionAotCatalogSize = {len(data)};\n"
        f"inline constexpr uint8_t kOrionAotCatalogSha256[] = {{{digest}}};\n"
        "\nstruct OrionAotCatalogEntry { const char* name; uint32_t offset, size, unpacked_size; };\n"
        f"inline constexpr OrionAotCatalogEntry kOrionAotCatalog[] = {{\n{images}\n}};\n",
        encoding="ascii",
    )
    print(f"Orion AOT apps catalog: {len(modules)} apps, {len(data)}/{capacity} flash bytes")


if __name__ == "__main__":
    main()
