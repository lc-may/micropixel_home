# SPDX-License-Identifier: Apache-2.0
"""The Orion flash workflow must embed and retain the SDK trust bundle."""

import importlib.util
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_orion_firmware", TOOLS / "prepare_orion_firmware.py")
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)


class SdkDescriptors:
    @staticmethod
    def read_descriptors(image):
        if image.startswith(b"firmware+"):
            return [{"start_addr": 9, "end_addr": 12}]
        return []

    @staticmethod
    def find_descriptor(entries, name):
        assert name == "CERTS"
        return next(iter(entries), None)


class PrepareOrionFirmwareTests(unittest.TestCase):
    def setUp(self):
        directory = self.enterContext(TemporaryDirectory())
        self.build = Path(directory) / "build"
        self.source = self.build / "build_out" / firmware.HOST_BINARY
        self.source.parent.mkdir(parents=True)
        config = self.build / "generated/autoconfig.h"
        config.parent.mkdir()
        config.write_text("#define CONFIG_FHOST 1\n#define CONFIG_X509_CERTIFICATE_BUNDLE 1\n")
        self.output = Path(directory) / "flash.bin"
        self.sdk = Path(directory) / "sdk"
        self.raw = b"firmware"
        self.with_certs = self.raw + b"+\x00\x01X"
        self.post_count = 0
        self.preparer = self.enterContext(patch.object(firmware.subprocess, "run", side_effect=self.run_sdk))
        self.enterContext(patch.object(firmware.runpy, "run_path", return_value={"CustomBinManager": SdkDescriptors}))

    def run_sdk(self, command, *, check, cwd=None):
        if command[0] == "cmake":
            self.assertEqual(command, ["cmake", "--build", str(self.build), "--target", "post_build"])
            self.post_count += 1
            self.source.write_bytes(self.with_certs)
        else:
            staged = Path(next(arg for arg in command if arg.startswith("--imgfile=")).split("=", 1)[1])
            self.assertEqual(staged.read_bytes(), self.with_certs)
            staged.write_bytes(self.with_certs + b"RF")

    def test_raw_build_is_post_processed_once(self):
        self.source.write_bytes(self.raw)
        for _ in range(2):
            firmware.prepare(self.build, self.output, self.sdk)
            self.assertEqual(self.output.read_bytes(), self.with_certs + b"RF")
        self.assertEqual(self.post_count, 1)

    def test_rejects_a_flash_image_that_loses_certs(self):
        self.source.write_bytes(self.with_certs)
        self.output.write_bytes(b"previous flash image")

        def remove_certs(command, *, check, cwd=None):
            path = Path(next(arg for arg in command if arg.startswith("--imgfile=")).split("=", 1)[1])
            path.write_bytes(self.raw)

        self.preparer.side_effect = remove_certs
        with self.assertRaisesRegex(ValueError, "lost CERTS"):
            firmware.prepare(self.build, self.output, self.sdk)
        self.assertEqual(self.output.read_bytes(), b"previous flash image")
