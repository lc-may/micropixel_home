# SPDX-License-Identifier: Apache-2.0
"""Exercise the Orion storage adapter with the pinned LittleFS and a NOR model."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("BL_SDK_BASE", PORT.parents[2] / "bouffalo_sdk"))
LITTLEFS = SDK / "components/fs/littlefs/littlefs"


class AppStoreFlashTests(unittest.TestCase):
    def test_capacity_and_atomic_replacement(self):
        if not (LITTLEFS / "lfs.c").is_file():
            self.skipTest("Bouffalo SDK LittleFS unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bflb_boot2.h").write_text(
                '#include <stdint.h>\nint bflb_boot2_partition_addr_active(const char*, uint32_t*, uint32_t*);\n'
            )
            (root / "bflb_flash.h").write_text(
                "#include <stdint.h>\nint bflb_flash_read(uint32_t, uint8_t*, uint32_t);\n"
            )
            (root / "FreeRTOS.h").write_text("#define portMAX_DELAY 0xffffffffU\n")
            (root / "semphr.h").write_text(
                "#include <assert.h>\n"
                "typedef unsigned StaticSemaphore_t; typedef StaticSemaphore_t* SemaphoreHandle_t;\n"
                "static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* s) {\n"
                "    *s = 0; return s; }\n"
                "static inline void xSemaphoreTakeRecursive(SemaphoreHandle_t s, unsigned timeout) {\n"
                "    (void)timeout; ++*s; }\n"
                "static inline void xSemaphoreGiveRecursive(SemaphoreHandle_t s) { assert(*s); --*s; }\n"
            )
            source = Path(__file__).with_name("app_store_flash_test.c")
            binary = root / "app_store_flash_test"
            subprocess.run(
                [shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(root), "-I", str(LITTLEFS), "-I", str(SDK / "components/fs/littlefs/port"),
                 "-I", str(PORT / "platform/storage"), "-I", str(PORT.parent / "common"),
                 "-I", str(PORT.parents[1] / "guest"), "-DLFS_THREADSAFE", "-DCONFIG_FREERTOS", "-DWASM_ENABLE_AOT=1",
                 str(source), str(PORT / "platform/storage/file_store.c"),
                 str(LITTLEFS / "lfs.c"), str(LITTLEFS / "lfs_util.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
            if factory := os.environ.get("ORION_FACTORY_APP_STORE"):
                subprocess.run([str(binary), factory], check=True)


if __name__ == "__main__":
    unittest.main()
