#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build Orion's embedded font for text supplied by network services."""

import argparse
import hashlib
import lzma
from pathlib import Path
import struct
from urllib.request import urlopen

from fontTools.ttLib import TTFont
from fontTools.unicodedata import script
from PIL import ImageFont


SOURCE_URL = (
    "https://raw.githubusercontent.com/adobe-fonts/source-han-sans/2.004R/"
    "OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
)
SOURCE_SHA256 = "84bbd4ace91d327b3ad1a581c688196278a4e41308520176f419180064e4af2b"
PIXEL_SIZE = 17
RECORD = struct.Struct("<IIBBBbb3x")
PUNCTUATION = ((0x20, 0xFF), (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x214F),
               (0x3000, 0x303F), (0x30FB, 0x30FB), (0xFF00, 0xFFEF))


def check_source(data):
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("expected Adobe Source Han Sans SC Regular 2.004R")


def write_file(path, payload):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(payload)
    temporary.replace(path)


def source_file(source, cache):
    if source or cache.is_file():
        path = source or cache
        check_source(path.read_bytes())
        return path
    with urlopen(SOURCE_URL, timeout=60) as response:
        data = response.read(20 * 1024 * 1024)
    check_source(data)
    write_file(cache, data)
    return cache


def make_font(source):
    with TTFont(source) as font:
        cmap = font.getBestCmap()
    han = {cp for cp in cmap if script(chr(cp)) == "Hani"}
    codepoints = sorted(han | {cp for cp in cmap if script(chr(cp)) == "Latn"
                               or any(start <= cp <= end for start, end in PUNCTUATION)})
    if not set(range(0x20, 0x7F)) <= set(codepoints) or len(han) < 30000:
        raise ValueError("source font is missing the expected Han or ASCII coverage")
    face = ImageFont.truetype(str(source), PIXEL_SIZE, layout_engine=ImageFont.Layout.BASIC)
    records = bytearray()
    bitmap = bytearray()
    for cp in codepoints:
        char = chr(cp)
        mask, (x, top) = face.getmask2(char, mode="1", anchor="ls")
        width, height = mask.size
        advance = round(face.getlength(char))
        records += RECORD.pack(cp, len(bitmap), advance, width, height, x, -top - height)
        bits = bytearray((width * height + 7) // 8)
        for index, pixel in enumerate(bytes(mask)):
            if pixel:
                bits[index // 8] |= 0x80 >> (index % 8)
        bitmap += bits
    return len(codepoints), len(han), bytes(records + bitmap)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("--source", type=Path, help="local copy of the pinned OpenType font")
    args = parser.parse_args()
    args.header.parent.mkdir(parents=True, exist_ok=True)
    source = source_file(args.source, args.header.with_name("SourceHanSansSC-Regular.otf"))
    count, han, raw = make_font(source)
    packed = lzma.compress(raw, check=lzma.CHECK_CRC32)
    rows = [", ".join(f"0x{byte:02x}" for byte in packed[offset:offset + 16])
            for offset in range(0, len(packed), 16)]
    output = (
        "// Generated from Adobe Source Han Sans SC Regular 2.004R, SIL OFL 1.1.\n#pragma once\n\n"
        f"static constexpr unsigned kCoverageCount = {count};\n"
        f"static constexpr unsigned kHanCount = {han};\n"
        f"static constexpr unsigned kCoverageBytes = {len(raw)};\n"
        "static constexpr unsigned char kCompressedCoverage[] = {\n    "
        + ",\n    ".join(rows) + "\n};\n"
    )
    write_file(args.header, output.encode("ascii"))
    print(f"Orion font: {count} codepoints ({han} Han), {len(packed)} flash bytes, {len(raw)} PSRAM bytes")


if __name__ == "__main__":
    main()
