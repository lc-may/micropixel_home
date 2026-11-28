# SPDX-License-Identifier: Apache-2.0
"""Validate device grouping across advertising packets and BR/EDR inquiry results."""

from pathlib import Path
import subprocess
import tempfile
import unittest


PORT = Path(__file__).resolve().parents[2]


class BluetoothScanTests(unittest.TestCase):
    def test_advertising_data_and_device_classes(self):
        adapter = PORT / "platform/wireless"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "bluetooth_scan_test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(adapter), "-I", str(PORT.parents[1] / "guest"),
                 str(Path(__file__).with_name("bluetooth_scan_test.c")),
                 str(adapter / "bluetooth_scan.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
