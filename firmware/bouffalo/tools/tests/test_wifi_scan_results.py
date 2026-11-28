# SPDX-License-Identifier: Apache-2.0
"""Verify Wi-Fi scan ordering and the offline reconnect lifecycle."""

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


    def test_reconnect_lifecycle(self):
        adapter = PORT / "platform/wireless"
        tests = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as directory:
            scratch = Path(directory)
            binary = scratch / "wifi_reconnect_test"
            for name in ("FreeRTOS.h", "task.h", "async_event.h", "bflb_core.h", "lfs_kv.h",
                         "net_al.h", "net_al_ext.h", "wifi_mgmr_ext.h", "mm.h"):
                (scratch / name).touch()
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DCONFIG_FHOST",
                 "-include", str(tests / "wifi_reconnect_test_sdk.h"),
                 "-I", str(scratch), "-I", str(PORT), "-I", str(adapter),
                 "-I", str(PORT.parents[1] / "guest"),
                 str(tests / "wifi_reconnect_test.c"), str(adapter / "wireless.c"),
                 str(adapter / "wifi_history.c"), str(adapter / "wifi_scan_results.c"), "-o", str(binary)],
                check=True,
            )
            for scenario in ("train", "reconnect"):
                subprocess.run([str(binary), scenario], cwd=scratch, check=True)
            migration = scratch / "migration"
            migration.mkdir()
            subprocess.run([str(binary), "migrate"], cwd=migration, check=True)
