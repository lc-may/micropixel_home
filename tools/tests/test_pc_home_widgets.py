#!/usr/bin/env python3
"""Three Guest publishers and Host widget commands over the actual Service ABI."""
from test_pc_market import close_home_app, open_home_app, open_home_market
import http.server
import json
import os
import subprocess
from pathlib import Path
import tempfile
import threading
import time

from PIL import Image
from test_pc_components import descriptor
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler, TOKEN, CSRF, images
from test_pc_market import ROOT, BINARY, navigate_x11
from test_pc_music import mp3


def main():
    images()
    LoginHandler.mode = "success"
    HomeHandler.audio, HomeHandler.avatar = mp3(20), LoginHandler.png
    HomeHandler.resolve_id = 1001
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HomeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = ROOT / "build/pc-home-widgets-tests"
    results.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(dir=results) as temporary:
            directory = Path(temporary)
            paths = []
            for name in ("weather", "clock"):
                path = descriptor(ROOT / f"build/pc-apps/widget_{name}/widget_{name}.pc.json",
                                  directory / f"{'a' if name == 'weather' else 'b'}-{name}.pc.json", f"test.widget.{name}")
                value = json.loads(path.read_text())
                value["core_abi"] = 131072
                value["requirements"] = {"schema_version": 2,
                    "display": {"layouts": ["square"], "min_width": 240, "min_height": 240},
                    "required": [], "optional": [], "any_of": [],
                    "services": {"lifecycle": 65536, "widget": 65536, "message": 65536},
                    "components": {"activity": True, "service": True, "background": ["task"], "widgets": 1}}
                path.write_text(json.dumps(value))
                paths.append(path)
            paths.append(descriptor(ROOT / "build/pc-apps/netease-music/netease-music.pc.json",
                                    directory / "c-music.pc.json", "micropixel.netease-music"))
            contract = descriptor(ROOT / "build/pc-apps/app_services_contract/app_services_contract.pc.json",
                                  directory / "contract.pc.json", "test.app-services-contract")
            value = json.loads(contract.read_text())
            value["core_abi"] = 131072
            value["requirements"] = json.loads(paths[0].read_text())["requirements"]
            contract.write_text(json.dumps(value))
            checked = subprocess.run([str(BINARY), "--headless", "--autostart", "--exit-on-app-complete", "--seconds", "5", str(contract)],
                                     cwd=ROOT, env=dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "contract-data")), capture_output=True, timeout=10)
            (results / "contract.log").write_bytes(checked.stdout + checked.stderr)
            assert checked.returncode == 0 and b"APP SERVICES CONTRACT PASS" in checked.stderr, checked.stderr.decode()
            trapped = subprocess.run([str(BINARY), "--headless", "--autostart", "--exit-on-app-complete", "--seconds", "5",
                                      "--guest-arg", "--bad-pointer", str(contract)], cwd=ROOT,
                                     env=dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "contract-data")), capture_output=True, timeout=10)
            (results / "bounds-trap.log").write_bytes(trapped.stdout + trapped.stderr)
            assert trapped.returncode == 1 and b"out of bounds memory access" in trapped.stderr, trapped.stderr.decode()
            pcm = results / "widgets.pcm"
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "storage"),
                       SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(pcm))

            def driver(click, event, wait_for, log_path, process, escape):
                def wait_new(marker, action):
                    offset = len(log_path.read_text())
                    action()
                    deadline = time.monotonic() + 5
                    while marker not in log_path.read_text()[offset:] and time.monotonic() < deadline:
                        time.sleep(.02)
                    assert marker in log_path.read_text()[offset:], log_path.read_text()
                def sound():
                    time.sleep(.1)
                    offset = pcm.stat().st_size
                    time.sleep(.3)
                    return any(pcm.read_bytes()[offset:])

                open_home_app(click, 0)
                wait_for("WIDGET_PROVIDER weather ready")
                event.key(0xffbe)
                open_home_app(click, 1)
                wait_for("WIDGET_PROVIDER clock ready")
                event.key(0xffbe)
                open_home_app(click, 2)
                wait_for("NETEASE_LOGIN authenticated")
                wait_for("NETEASE_HOME playlists:")
                click(120, 680)
                wait_for("NETEASE_HOME recommendations: 30")
                click(140, 220)
                wait_for("NETEASE_GUEST selected: 1001")
                wait_for("Audio playing:")
                event.key(0xffbe)
                wait_for("HOME_WIDGET snapshot: app=0 template=1")
                wait_for("HOME_WIDGET snapshot: app=1 template=2")
                wait_for("HOME_WIDGET snapshot: app=2 template=3")
                assert sound()
                wait_new("HOME_MEDIA paused=1", lambda: click(548, 606))
                assert not sound(), "widget pause did not stop PCM"
                wait_new("HOME_MEDIA paused=0", lambda: click(548, 606))
                assert sound(), "widget resume did not restore PCM"
                HomeHandler.resolve_id = 1002
                wait_new("NETEASE_GUEST selected: 1002", lambda: click(636, 606))
                wait_new("WIDGET_PROVIDER refreshed", lambda: click(660, 96))
                click(200, 606)
                wait_for("APP_SHELL foreground: micropixel.netease-music reused=1")
                assert log_path.read_text().count("NETEASE_GUEST started:") == 1
                assert log_path.read_text().count("NETEASE_LOGIN authenticated") == 1
                event.key(0xffbe)
                open_home_app(click, 0)
                wait_new("APP_SHELL stopped: test.widget.weather", lambda: close_home_app(click, escape))
                assert "APP_SHELL stopped: test.widget.clock" not in log_path.read_text()
                assert sound(), "closing weather stopped music"
                wait_new("NETEASE_ACTIVITY resumed", lambda: click(200, 606))
                wait_new("APP_SHELL stopped: micropixel.netease-music", lambda: close_home_app(click, escape))
                assert not sound(), "closing music left its PCM alive"
                assert "APP_SHELL stopped: test.widget.clock" not in log_path.read_text()

            args = ["--home", "--multi-app", "--guest-arg", "--login", "--guest-arg", "--test-origin",
                    "--guest-arg", f"http://127.0.0.1:{server.server_port}", *paths]
            navigate_x11(env, results, args, driver=driver, seconds=24, wait_seconds=6)
            log = (results / "navigation.log").read_text()
            assert "Guest failed:" not in log and TOKEN not in log and CSRF not in log, log
            with Image.open(results / "navigation.png") as image:
                assert image.convert("RGB").getpixel((400, 510)) != (11, 20, 35), "clock widget missing"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS three-source Home widgets, queued pause/resume/next/refresh, same-session state and independent app stop")


if __name__ == "__main__":
    main()
