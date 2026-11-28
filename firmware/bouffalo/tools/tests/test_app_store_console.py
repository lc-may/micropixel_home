# SPDX-License-Identifier: Apache-2.0
"""UART timeouts must preserve fragments and never publish a partial application list."""

from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
with patch.object(sys, "path", [str(TOOLS), *sys.path]):
    spec = importlib.util.spec_from_file_location("orion_apps", TOOLS / "orion_apps.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)


class FakeUart:
    def __init__(self, fragments):
        self.fragments = iter(fragments)
        self.writes = []

    @property
    def in_waiting(self):
        return 8192

    def read(self, size):
        return next(self.fragments, b"")

    def write(self, data):
        self.writes.append(data)


class StoreConsoleTests(unittest.TestCase):
    def test_list_survives_partial_lines_and_idle_reads(self):
        uart = FakeUart([b"Orion LCD fra", b"", b"mes=52, PCM=0\r\n",
                         b"MPX APP micro", b"", b"pixel.settings 0.1.0 stopped\r\n",
                         b"MPX STORE list ", b"", b"ok 1 3313664\r\n"])
        console = module.Console(module.UartLines(uart))
        console.wait_ready(timeout=0.1)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(console.exchange("list"), "1 3313664")
        self.assertEqual(output.getvalue(), "micropixel.settings 0.1.0 stopped\n")
        self.assertEqual(uart.writes, [b"\rmpx status\r", b"\rmpx bundle list\r"])

    def test_incomplete_list_does_not_print_entries(self):
        uart = FakeUart([b"MPX APP micropixel.settings 0.1.0 stopped\nMPX STORE list ok 2 3313664\n"])
        output = io.StringIO()
        with redirect_stdout(output), self.assertRaisesRegex(TimeoutError, "Incomplete"):
            module.Console(module.UartLines(uart)).exchange("list")
        self.assertEqual(output.getvalue(), "")

    def test_completion_can_share_a_line_with_pending_reply(self):
        uart = FakeUart([b"MPX STORE apply pend", b"ingMPX STORE apply ok\r\n"])
        console = module.Console(module.UartLines(uart))
        self.assertEqual(console.exchange("apply", timeout=0.1), "")
