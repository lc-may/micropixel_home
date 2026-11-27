# SPDX-License-Identifier: Apache-2.0
"""Verify scan capacity, RSSI ordering and the targets chosen for a join."""

from pathlib import Path
import subprocess
import tempfile
import unittest


PORT = Path(__file__).resolve().parents[2]


class WifiScanTests(unittest.TestCase):
    def test_sort_and_band_preference(self):
        adapter = PORT / "platform/wireless"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "wifi_scan_results_test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(adapter), "-I", str(PORT.parents[1] / "guest"),
                 str(Path(__file__).with_name("wifi_scan_results_test.c")),
                 str(adapter / "wifi_scan_results.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
