#!/usr/bin/env python3
"""Real Wasm component wire contract, concurrent HTTP, HMI and legacy policy."""
from test_pc_market import open_home_app, open_home_market
import array
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time

from PIL import Image
from test_pc_market import navigate_x11, ROOT, BINARY


class Handler(http.server.BaseHTTPRequestHandler):
    arrived = threading.Barrier(2)
    completed = 0

    def log_message(self, *_):
        pass

    def do_GET(self):
        Handler.arrived.wait(timeout=8)  # A must keep running while B opens.
        body = b"independent request completed"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        Handler.completed += 1


def descriptor(source, output, app_id):
    value = json.loads(source.read_text())
    value["app_id"] = app_id
    for key in ("wasm", "resources"):
        if value.get(key):
            value[key] = str((source.parent / value[key]).resolve())
    output.write_text(json.dumps(value))
    return output


def main():
    results = ROOT / "build/pc-components-tests"
    results.mkdir(parents=True, exist_ok=True)
    base = ROOT / "build/pc-apps/components-demo/components-demo.pc.json"
    with tempfile.TemporaryDirectory(dir=results) as temporary:
        directory = Path(temporary)
        env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "storage"))
        contract = json.loads((ROOT / "build/pc-apps/components_contract/components_contract.pc.json").read_text())
        contract["wasm"] = str((ROOT / "build/pc-apps/components_contract/components_contract.wasm").resolve())
        contract["requirements"] = json.loads(base.read_text())["requirements"]
        contract["core_abi"] = 131072
        contract["requirements"]["components"] = {"activity": True, "service": False, "background": [], "widgets": 0}
        path = directory / "contract.pc.json"
        path.write_text(json.dumps(contract))
        run = subprocess.run([str(BINARY), "--headless", "--autostart", "--exit-on-app-complete", "--seconds", "5", str(path)],
                             cwd=ROOT, env=env, capture_output=True, timeout=10)
        (results / "contract.log").write_bytes(run.stdout + run.stderr)
        assert run.returncode == 0 and b"COMPONENT CONTRACT PASS" in run.stderr, run.stderr.decode()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        paths = [descriptor(base, directory / f"{name}.pc.json", f"test.components.{name}") for name in ("a", "b")]
        paths.append(descriptor(ROOT / "build/pc-apps/legacy_heartbeat/legacy_heartbeat.pc.json",
                                directory / "z-legacy.pc.json", "test.legacy"))
        recording = results / "background.pcm"
        env.update(SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(recording))
        color = []

        def driver(click, event, wait_for, log_path, process, escape):
            def text():
                return log_path.read_text(errors="replace")
            def marker(app, message):
                return f"[Guest test.components.{app} level=2] COMP {message}"
            def count(app, message):
                return text().count(marker(app, message))
            def wait_count(app, message, expected):
                deadline = time.monotonic() + 4
                while count(app, message) < expected and time.monotonic() < deadline:
                    time.sleep(0.02)
                assert count(app, message) >= expected, text()
            open_home_app(click, 0)
            wait_for(marker("a", "heartbeat=25"))
            event.key(0xffbe)
            open_home_app(click, 1)
            wait_for(marker("b", "resumed"))
            wait_for(marker("a", "HTTP bytes="))
            wait_for(marker("b", "HTTP bytes="))
            assert Handler.completed == 2
            wait_for(marker("b", "heartbeat=25"))
            before = count("a", "heartbeat=")
            event.key(0xff0d)
            wait_for(marker("b", "key received"))
            assert count("a", "key received") == 0
            time.sleep(0.35)
            assert count("a", "heartbeat=") > before, text()
            event.key(0xffbe)
            open_home_app(click, 2)  # Legacy v1 app: foreground only in --multi-app.
            wait_for("LEGACY heartbeat")
            event.key(0xffbe)
            time.sleep(0.2)
            stopped = text().count("LEGACY heartbeat")
            time.sleep(0.4)
            assert text().count("LEGACY heartbeat") == stopped, text()
            open_home_app(click, 2)
            time.sleep(0.3)
            assert text().count("LEGACY heartbeat") > stopped, text()
            event.key(0xffbe)
            open_home_app(click, 0)
            wait_count("a", "resumed", 2)
            assert count("a", "create=") == 1
            click(120, 550)
            wait_for(marker("a", "destroyed"))
            time.sleep(0.2)
            open_home_app(click, 0)
            wait_for(marker("a", "create=2"))
            wait_count("a", "resumed", 3)
            click(350, 550)
            wait_for(marker("a", "service stopped"))
            time.sleep(0.2)
            stopped = count("a", "heartbeat=")
            before = count("b", "heartbeat=")
            time.sleep(0.35)
            assert count("a", "heartbeat=") == stopped and count("b", "heartbeat=") > before, text()
            click(350, 550)
            wait_count("a", "service started", 2)
            event.key(0xffbe)
            open_home_app(click, 1)
            wait_count("b", "resumed", 2)
            click(580, 550)
            wait_for("APP_SHELL stopped: test.components.b")
            before = count("a", "heartbeat=")
            time.sleep(0.35)
            assert count("a", "heartbeat=") > before, text()
            open_home_app(click, 0)
            wait_count("a", "resumed", 4)
            assert count("a", "create=") == 2
            color.append(int(re.search(r"test.components.a level=2\] COMP color=(\d+)", text()).group(1)))
            assert "Guest failed:" not in text(), text()
            # Let the process deadline capture the visible A frame and shut down.

        try:
            navigate_x11(env, results, ["--multi-app", "--home", "--guest-arg", "--test-url", "--guest-arg",
                         f"http://127.0.0.1:{server.server_port}/resource", *paths], driver=driver, seconds=13, wait_seconds=5)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        with Image.open(results / "navigation.png") as image:
            expected = tuple((color[0] >> shift) & 255 for shift in (16, 8, 0))
            assert image.convert("RGB").getpixel((100, 200)) == expected, "foreground texture ownership changed"
        samples = array.array("h", recording.read_bytes())
        assert len(samples) > 48000 and any(samples[-48000:]), "background audio stopped after closing B"
    print("PASS components wire contract, concurrent Guest HTTP/PCM, input/texture routing, legacy suspend and Activity/Service HMI")


if __name__ == "__main__":
    main()
