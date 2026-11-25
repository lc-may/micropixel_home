#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and embed the SDK audio conformance Guest."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wamrc', required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    directory = args.output.parent
    directory.mkdir(parents=True, exist_ok=True)
    name = args.source.stem
    subprocess.run([sys.executable, str(ROOT/'tools/micropixel'), 'build', str(args.source),
                    '--name', name, '--profile', 'release', '--aot-target', 'riscv32-ilp32f',
                    '--output-dir', str(directory)], cwd=ROOT,
                   env=dict(os.environ, WAMRC=args.wamrc), check=True)
    data = (directory/f'{name}.aot').read_bytes()
    if data[:8] != b'\x00aot\x06\x00\x00\x00':
        raise SystemExit('The Orion audio test requires WAMR AOT v6')
    rows = [', '.join(f'0x{byte:02x}' for byte in data[i:i+16]) for i in range(0, len(data), 16)]
    args.output.write_text('// Generated audio Guest AOT; do not commit.\n'
                           'alignas(4) static unsigned char audio_guest_aot[] = {\n'
                           + ',\n'.join(rows) + '\n};\n')


if __name__ == '__main__':
    main()
