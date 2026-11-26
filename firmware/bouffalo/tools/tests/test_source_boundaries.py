# SPDX-License-Identifier: Apache-2.0
"""Keep the Bouffalo build and shared Host sources usable in a subset checkout."""

import os
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[3]


class SourceBoundaries(unittest.TestCase):
    def test_bouffalo_sources_do_not_reference_espressif_tree(self):
        forbidden = re.compile(r"""(?:[/\\]|["'])espressif(?:[/\\]|["'])""")
        source_suffixes = {".c", ".cmake", ".cpp", ".h", ".hpp", ".py"}
        for root in (FIRMWARE / "bouffalo", FIRMWARE / "common"):
            for directory, children, files in os.walk(root):
                children[:] = [child for child in children if not child.startswith("build")
                               and child not in {"third_party", "__pycache__"}]
                for name in files:
                    source = Path(directory) / name
                    if source.suffix == ".py" and source.parent.name == "tests":
                        continue
                    if source.suffix in source_suffixes or name in {"CMakeLists.txt", "Makefile"}:
                        with self.subTest(source=str(source.relative_to(FIRMWARE))):
                            self.assertNotRegex(source.read_text(encoding="utf-8"), forbidden)
