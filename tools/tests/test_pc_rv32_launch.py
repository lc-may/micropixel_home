#!/usr/bin/env python3
"""Exercise the documented pc.sh launcher with real RV32 Home services."""
import http.server
import json
import os
import subprocess
import tempfile
import threading
import time
from pc_test_backend import ROOT


class Weather(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        body = json.dumps({"current": {"time": int(time.time()), "temperature_2m": 20.5, "weather_code": 1}}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    results = ROOT / "build/pc-rv32-launch-tests"
    results.mkdir(exist_ok=True)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Weather)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(dir=results) as directory:
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=directory)
            command = ["bash", "tools/pc.sh", "run", "--home", "--rv32", "--headless", "--seconds", "8",
                       "--screenshot", str(results / "home.png"), "--guest-arg", "--weather-origin", "--guest-arg",
                       f"http://127.0.0.1:{server.server_port}/weather"]
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=300)
            log = result.stderr.decode(errors="replace")
            (results / "launch.log").write_text(log)
            assert result.returncode == 0, log
            assert log.count("Bundle opened: app=") == 3 and log.count("RV32 machine started:") == 2, log
            assert "CLOCK service ready" in log and "WEATHER refresh success" in log, log
            assert "HOME_WIDGET snapshot: app=0 template=2" in log, log
            assert "HOME_WIDGET snapshot: app=2 template=1" in log, log
            assert (results / "home.png").is_file()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS pc.sh run --home --rv32: three validated bundles, cold Clock/Weather services and visible Home widgets")


if __name__ == "__main__":
    main()
