#!/usr/bin/env python3
"""Check visible retained text in a real Guest after the LVGL hall has rendered."""
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def main():
    subprocess.run(["python3", "tools/pc.py", "guest/apps/snake"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix="pc-render-", dir=ROOT / "build") as directory:
        output = Path(directory)
        screenshot = output / "snake.png"
        env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(output / "storage"))
        subprocess.run([str(ROOT / "build/pc/micropixel-pc"), "--headless", "--autostart", "--seconds", "2",
                        "--screenshot", str(screenshot), str(ROOT / "build/pc-apps/snake/snake.pc.json")],
                       cwd=ROOT, env=env, timeout=15, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with Image.open(screenshot) as image:
            assert image.size == (720, 720)
            # START GAME sits inside the opaque green button. Its retained black
            # glyphs disappeared when composition ran concurrently with LVGL.
            pixels = list(image.convert("RGB").crop((230, 330, 490, 365)).getdata())
            assert sum(max(pixel) < 40 for pixel in pixels) > 150, "START GAME text was not painted"
            assert sum(pixel[1] > 150 for pixel in pixels) > 1000, "Start button was not painted"
            title = list(image.convert("RGB").crop((0, 20, 150, 55)).getdata())
            assert sum(pixel[1] > 100 and pixel[1] > pixel[0] * 1.5 for pixel in title) > 100, "Guest title was not painted"
    print("PASS PC visible Guest text, retained layers and app launch after the original hall cards")


if __name__ == "__main__":
    main()
