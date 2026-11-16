#!/usr/bin/env python3
"""News feed rendering, cached read state and music coexistence in real SDL windows."""

import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from PIL import Image
from pc_test_backend import ROOT, app_path
from test_pc_components import descriptor
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler, images
from test_pc_market import BINARY, navigate_x11, open_home_app, close_home_app
from test_pc_music import mp3
from test_pc_panel import Handler as PanelHandler


class Handler(PanelHandler):
    mode = "good"
    delay = 0
    requests = 0

    def do_GET(self):
        if self.path != "/news":
            return super().do_GET()
        Handler.requests += 1
        assert "MicroPixel/1.0" in self.headers.get("User-Agent", "")
        mode = Handler.mode
        time.sleep(Handler.delay)
        if mode == "failure":
            data, status = b"unavailable", 503
        else:
            data = json.dumps(
                {
                    "status": "success",
                    "padding": "x" * 110000,
                    "data": []
                    if mode == "empty"
                    else [
                        {
                            "Title": f"第{i + 1}条：城市公共服务与科技创新观察，持续关注身边发生的新闻与变化",
                            "HotValue": str(21000000 - i * 10000),
                        }
                        for i in range(50)
                    ],
                },
                ensure_ascii=False,
            ).encode()
            status = 200
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    rv32 = "--rv32" in sys.argv
    results = ROOT / ("build/news-rv32-tests" if rv32 else "build/news-tests")
    results.mkdir(exist_ok=True)
    if "--live" in sys.argv:
        with tempfile.TemporaryDirectory(dir=results) as directory:
            env = dict(
                os.environ,
                MICROPIXEL_PC_DATA_DIR=str(Path(directory) / "data"),
                SDL_AUDIODRIVER="dummy",
            )
            with (results / "live.log").open("w") as log:
                run = subprocess.run(
                    [
                        str(BINARY),
                        "--headless",
                        "--autostart",
                        "--seconds",
                        "22",
                        "--screenshot",
                        str(results / "live.png"),
                        str(app_path("news")),
                    ],
                    env=env,
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=35,
                )
            text = (results / "live.log").read_text()
            assert (
                run.returncode == 0
                and "NEWS refresh success: 50" in text
                and "Guest failed:" not in text
            ), text
        print(
            "PASS live Toutiao HTTPS through Guest Network SDK: 50 headlines and heat values"
        )
        return
    images()
    LoginHandler.mode = "success"
    HomeHandler.avatar, HomeHandler.audio = LoginHandler.png, mp3(90)
    HomeHandler.resolve_id = 1001
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(dir=results) as directory:
            output = Path(directory)
            paths = [
                descriptor(
                    app_path(name), output / f"{i}-{name}.pc.json", f"micropixel.{name}"
                )
                for i, name in enumerate(
                    ("clock-alarm", "netease-music", "news", "weather")
                )
            ]
            pcm = results / "music.pcm"
            env = dict(
                os.environ,
                MICROPIXEL_PC_DATA_DIR=str(output / "data"),
                SDL_AUDIODRIVER="disk",
                SDL_DISKAUDIOFILE=str(pcm),
            )
            origin = f"http://127.0.0.1:{server.server_port}"
            args = [
                "--home",
                *paths,
                "--guest-arg",
                "--login",
                "--guest-arg",
                "--test-origin",
                "--guest-arg",
                origin,
                "--guest-arg",
                "--weather-origin",
                "--guest-arg",
                origin + "/weather",
                "--guest-arg",
                "--news-origin",
                "--guest-arg",
                origin + "/news",
            ]

            def drive(click, event, wait_for, log_path, process, escape):
                def fresh(marker, action):
                    at = len(log_path.read_text())
                    action()
                    end = time.monotonic() + 25
                    while (
                        marker not in log_path.read_text()[at:]
                        and time.monotonic() < end
                    ):
                        time.sleep(0.04)
                    assert marker in log_path.read_text()[at:], log_path.read_text()[
                        -18000:
                    ]

                def capture(name, ready=None):
                    end = time.monotonic() + 12
                    while True:
                        fresh("Screenshot:", lambda: event.key(0xFFC9))
                        target = results / (name + ".png")
                        target.write_bytes((output / "navigation.png").read_bytes())
                        with Image.open(target) as shot:
                            if ready is None or ready(shot.convert("RGB")):
                                return
                        assert time.monotonic() < end, name + " not rendered"
                        time.sleep(0.1)

                def ready(shot):
                    return (
                        sum(
                            r > 180 and g > 180 and b > 180
                            for r, g, b in shot.crop((635, 25, 675, 60)).getdata()
                        )
                        > 40
                    )

                def sound():
                    start = pcm.stat().st_size
                    time.sleep(0.3)
                    with pcm.open("rb") as f:
                        f.seek(start)
                        return any(f.read())

                wait_for("WEATHER service ready")
                click(100, 566)
                wait_for("NETEASE_HOME playlists:")
                click(128, 680)
                wait_for("NETEASE_HOME recommendations: 30")
                click(140, 220)
                wait_for("Audio playing:")
                fresh("HOME visible:", escape)
                capture("home-music-compact")
                with Image.open(results / "home-music-compact.png") as shot:
                    rgb = shot.convert("RGB")
                    amber = sum(
                        r > 190 and 110 < g < 195 and b < 130
                        for r, g, b in rgb.crop((159, 579, 327, 639)).getdata()
                    )
                    assert 1800 < amber < 3200, (
                        "compact rectangular play button",
                        amber,
                    )
                    assert rgb.getpixel((322, 610)) == (36, 42, 47), (
                        "rounded transport background missing"
                    )
                fresh("HOME_MEDIA paused=1", lambda: click(207, 601))
                assert not sound()
                fresh("HOME_MEDIA paused=0", lambda: click(207, 601))
                assert sound()
                Handler.delay = 3
                open_home_app(click, 2)
                wait_for("NEWS refresh started")
                capture("news-loading")
                wait_for("NEWS refresh success: 50")
                capture("news-list", ready)
                assert sound(), "news stopped background music"
                opens = log_path.read_text().count("NEWS detail opened:")
                event(6, 0, 500, 600)
                event(4, 1, 500, 600)
                for i in range(1, 9):
                    event(6, 0, 500, 600 - 385 * i // 8)
                event(5, 1, 500, 215)
                assert log_path.read_text().count("NEWS detail opened:") == opens, (
                    "scroll selected a headline"
                )
                capture("news-scrolled")
                fresh("NEWS detail opened: 3", lambda: click(150, 220))
                capture("news-detail")
                fresh("NEWS detail closed", lambda: click(42, 42))
                fresh("NEWS filter read", lambda: click(590, 106))
                capture("news-read")
                fresh("NEWS filter unread", lambda: click(360, 106))
                Handler.delay = 0
                Handler.mode = "failure"
                fresh("NEWS refresh failed", lambda: click(654, 42))
                capture("news-cached", ready)
                assert sound()
                Handler.mode = "empty"
                fresh("NEWS refresh success: 0", lambda: click(654, 42))
                capture("news-empty", ready)
                Handler.mode = "good"
                fresh("NEWS refresh success: 50", lambda: click(654, 42))
                capture("news-restored", ready)
                Handler.delay = 5
                fresh("NEWS refresh started", lambda: click(654, 42))
                fresh("NEWS activity stopped", escape)
                assert sound(), "leaving news stopped music"
                Handler.mode = "failure"
                Handler.delay = 0
                fresh("NEWS refresh failed", lambda: open_home_app(click, 2))
                fresh(
                    "APP_SHELL stopped: micropixel.news",
                    lambda: close_home_app(click, escape),
                )
                assert sound(), "closing news stopped music"
                fresh("NEWS refresh failed", lambda: open_home_app(click, 2))
                wait_for("NEWS cache loaded: 12")
                capture("news-offline-cache", ready)
                fresh("NEWS filter read", lambda: click(590, 106))
                capture("news-read-restored")
                assert sound()
                escape()

            navigate_x11(
                env, output, args, drive, seconds=95 if rv32 else 75, wait_seconds=25
            )
            text = (output / "navigation.log").read_text()
            (results / "ui.log").write_text(text)
            assert (
                "Guest failed:" not in text
                and "Guest initialization failed:" not in text
            ), text[-16000:]
            assert text.count("APP_SHELL started: micropixel.news") == 2
            assert text.count("NETEASE_LOGIN authenticated") == 1
            assert Handler.requests >= 6, Handler.requests
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print(
        "PASS news 130KB feed, skeleton, scroll, full-title detail, read filters, empty/error/cache/restart, compact Home controls and four-app background PCM"
    )


if __name__ == "__main__":
    main()
