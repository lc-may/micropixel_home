#!/usr/bin/env python3
"""Real Wasm composite UI ownership, identity and gesture regression."""
import os
from pathlib import Path
import tempfile
import time

from test_pc_market import ROOT, navigate_x11


def main():
    results = ROOT / "build/pc-ui-component-tests"
    results.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ui-", dir=results) as directory:
        directory = Path(directory)
        env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "data"))

        def driver(click, event, wait_for, log_path, process, escape):
            wait_for("UI COMPONENTS ready")

            def assert_counts(body, trailing):
                time.sleep(0.15)
                text = log_path.read_text()
                assert text.count("UI body=") == body, text
                assert text.count("UI trailing=") == trailing, text

            # A translated parent must not change click/scroll hit testing.
            event(6, 0, 100, 400)
            event(4, 1, 100, 400)
            event(6, 0, 100, 280)
            event(5, 1, 100, 280)
            wait_for("UI scroll=120")
            assert_counts(0, 0)
            click(100, 260)
            wait_for("UI body=1")
            wait_for("UI key=1003")
            click(410, 260)
            wait_for("UI trailing=1")
            assert_counts(1, 1)

            # Replace the dataset while the same physical pointer is held.
            event(6, 0, 100, 260)
            event(4, 1, 100, 260)
            event.key(0xFF0D)
            wait_for("UI dataset=2")
            event(5, 1, 100, 260)
            assert_counts(1, 1)
            click(100, 260)
            wait_for("UI body=2")
            wait_for("UI key=2003")

            # Crossing the drag threshold without changing offset still cancels a tap.
            event.key(0xFF52)
            wait_for("UI scroll reset")
            event(6, 0, 100, 235)
            event(4, 1, 100, 235)
            event(6, 0, 180, 235)
            event(5, 1, 180, 235)
            wait_for("UI scroll=0")
            assert_counts(2, 1)

            click(530, 125)
            wait_for("UI selection=1")
            event.key(0xFF51)
            wait_for("UI tab disabled")
            click(530, 125)
            click(650, 42)
            wait_for("UI header action=1")
            event.key(0xFF53)
            wait_for("UI header disabled")
            click(650, 42)
            text = log_path.read_text()
            assert "UI selection=2" not in text, text
            assert "UI header action=2" not in text, text
            assert "Guest failed:" not in text, text

        navigate_x11(env, directory, ["--autostart", "build/pc-apps/ui_components/ui_components.pc.json"],
                     driver, seconds=16, wait_seconds=5)
        final_log = (directory / "navigation.log").read_text()
        assert "Guest failed:" not in final_log, final_log
        (results / "widgets.log").write_text(final_log)
        (results / "widgets.png").write_bytes((directory / "navigation.png").read_bytes())
    print("PASS composite UI ownership/rollback/text compaction, stable row keys/rebind cancellation, translated scroll, "
          "body/action isolation, disabled controls and bounded node pool")


if __name__ == "__main__":
    main()
