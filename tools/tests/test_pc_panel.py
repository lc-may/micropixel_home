#!/usr/bin/env python3
"""Actual panel UI, Notes persistence and common native/RV32 business interactions."""

import http.server
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.parse
from PIL import Image
from pc_test_backend import ROOT, app_path
from test_pc_components import descriptor
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler, images, TOKEN, CSRF
from test_pc_market import navigate_x11
from test_pc_music import mp3


class Handler(HomeHandler):
    weather_code = 1
    offline = False
    weather_queries = []

    def do_GET(self):
        if urllib.parse.urlsplit(self.path).path == "/weather":
            Handler.weather_queries.append(
                urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            )
            if Handler.offline:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            now = int(time.time())
            hour = now // 3600 * 3600
            day = (now + 28800) // 86400 * 86400 - 28800
            return self.data(
                {
                    "hourly": {
                        "time": [hour + i * 3600 for i in range(5)],
                        "temperature_2m": [21.5, 22, 23, 22, 20],
                        "weather_code": [1, 0, 2, 3, 61],
                    },
                    "daily": {
                        "time": [day + i * 86400 for i in range(4)],
                        "temperature_2m_min": [18, 17, 16, 15],
                        "temperature_2m_max": [24, 25, 23, 21],
                        "weather_code": [1, 2, 3, 61],
                    },
                    "current": {
                        "time": int(time.time()),
                        "temperature_2m": 21.5,
                        "weather_code": Handler.weather_code,
                        "relative_humidity_2m": 65,
                        "wind_speed_10m": 13.2,
                    },
                }
            )
        return super().do_GET()


def main():
    import sys

    rv32 = "--rv32" in sys.argv
    results = ROOT / ("build/panel-rv32-tests" if rv32 else "build/panel-tests")
    results.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=results) as directory:
        directory = Path(directory)
        with (results / "planner.log").open("w") as log:
            result = subprocess.run(
                [str(ROOT / "build/pc/pc-planner-test"), str(app_path("clock-alarm"))],
                env=dict(
                    os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "planner-data")
                ),
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=100,
            )
        text = (results / "planner.log").read_text()
        assert (
            result.returncode == 0
            and "NOTE completed" in text
            and "NOTE deleted" in text
            and "Guest failed:" not in text
        ), text[-18000:]
        images()
        LoginHandler.mode = "success"
        HomeHandler.audio = mp3(40)
        HomeHandler.avatar = LoginHandler.png
        HomeHandler.resolve_id = 1001
        HomeHandler.like_target = 1001
        HomeHandler.liked = {1002}
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            paths = [
                descriptor(
                    app_path(name),
                    directory / f"{i}-{name}.pc.json",
                    f"micropixel.{name}",
                )
                for i, name in enumerate(("clock-alarm", "netease-music", "weather"))
            ]
            pcm = results / "playback.pcm"
            env = dict(
                os.environ,
                MICROPIXEL_PC_DATA_DIR=str(directory / "ui-data"),
                SDL_AUDIODRIVER="disk",
                SDL_DISKAUDIOFILE=str(pcm),
            )

            def driver(click, event, wait_for, log_path, process, escape):
                def fresh(marker, action, timeout=30):
                    offset = len(log_path.read_text())
                    action()
                    end = time.monotonic() + timeout
                    while (
                        marker not in log_path.read_text()[offset:]
                        and time.monotonic() < end
                    ):
                        time.sleep(0.03)
                    assert marker in log_path.read_text()[offset:], (
                        log_path.read_text()[-18000:]
                    )

                def capture(name, ready=None):
                    deadline = time.monotonic() + 10
                    while True:
                        fresh("Screenshot:", lambda: event.key(0xFFC9))  # F12
                        target = results / f"{name}.png"
                        target.write_bytes((directory / "navigation.png").read_bytes())
                        with Image.open(target) as shot:
                            if ready is None or ready(shot.convert("RGB")):
                                return
                        assert time.monotonic() < deadline, (
                            f"{name} did not render the confirmed state"
                        )
                        time.sleep(0.05)

                def sound():
                    at = pcm.stat().st_size
                    time.sleep(0.3)
                    with pcm.open("rb") as f:
                        f.seek(at)
                        return any(f.read())

                def weather_ready(shot):
                    # A service reply precedes the Activity's next render. Wait
                    # for its enabled refresh icon and populated temperature.
                    return (
                        sum(
                            r > 180 and g > 180 and b > 180
                            for r, g, b in shot.crop((635, 25, 675, 60)).getdata()
                        )
                        > 40
                        and sum(
                            r > 180 and g > 180 and b > 180
                            for r, g, b in shot.crop((24, 90, 340, 210)).getdata()
                        )
                        > 1500
                    )

                wait_for("WEATHER refresh success")
                wait_for("CLOCK service ready")
                wait_for("HOME_WIDGET snapshot: app=2 template=1")
                capture(
                    "home",
                    lambda shot: (
                        sum(
                            r > 180 and g > 180 and b > 180
                            for r, g, b in shot.crop((504, 148, 684, 212)).getdata()
                        )
                        > 1000
                    ),
                )
                fresh("WEATHER activity created", lambda: click(570, 185))
                capture("weather", weather_ready)
                click(150, 42)
                capture("weather-popular-cities")
                fresh("WEATHER refresh success", lambda: click(360, 270))
                wait_for("WEATHER city selected: 北京")
                capture("weather-beijing", weather_ready)

                def drag(start, end):
                    event(6, 0, 570, start)
                    event(4, 1, 570, start)
                    for i in range(1, 9):
                        event(6, 0, 570, start + (end - start) * i // 8)
                    event(5, 1, 570, end)
                    time.sleep(0.2)

                click(150, 42)
                click(530, 138)
                capture("weather-provinces")
                selections = log_path.read_text().count("WEATHER city selected:")
                drag(640, 260)
                drag(640, 260)
                assert (
                    log_path.read_text().count("WEATHER city selected:") == selections
                )
                click(
                    130, 534
                )  # Xinjiang: region 30, at the bottom of the scrolled region list.
                capture("weather-xinjiang-cities")
                drag(640, 260)  # Recycle city rows, then return to the first row.
                drag(260, 640)
                assert (
                    log_path.read_text().count("WEATHER city selected:") == selections
                )
                fresh("WEATHER refresh success", lambda: click(130, 270))
                wait_for("WEATHER city selected: 乌鲁木齐")
                query = Handler.weather_queries[-1]
                assert (
                    43 < float(query["latitude"][0]) < 44
                    and 87 < float(query["longitude"][0]) < 88
                ), query
                capture("weather-urumqi", weather_ready)
                previous = (
                    Image.open(results / "weather-urumqi.png")
                    .convert("RGB")
                    .crop((480, 96, 597, 213))
                    .tobytes()
                )
                for code, name in (
                    (0, "sunny"),
                    (3, "cloudy"),
                    (45, "fog"),
                    (53, "drizzle"),
                    (61, "rain"),
                    (71, "snow"),
                    (95, "thunder"),
                ):
                    Handler.weather_code = code
                    fresh("WEATHER refresh success", lambda: click(654, 42))
                    capture(
                        "weather-" + name,
                        lambda shot: (
                            weather_ready(shot)
                            and (shot.crop((480, 96, 597, 213)).tobytes() != previous)
                        ),
                    )
                    previous = (
                        Image.open(results / ("weather-" + name + ".png"))
                        .convert("RGB")
                        .crop((480, 96, 597, 213))
                        .tobytes()
                    )
                Handler.offline = True
                fresh("WEATHER refresh failed", lambda: click(654, 42))
                capture("weather-cached", weather_ready)
                Handler.offline = False
                Handler.weather_code = 1
                fresh("WEATHER refresh success", lambda: click(654, 42))
                event.key(0xFFBE)
                fresh("CLOCK calendar opened", lambda: click(540, 375))
                capture("calendar")
                click(42, 42)
                fresh("CLOCK notes opened", lambda: click(530, 657))
                capture("notes-empty")
                fresh("CLOCK editor opened", lambda: click(360, 664))
                click(270, 150)
                for char in "meeting":
                    event.key(ord(char))
                event.key(0xFF0D)
                capture("note-editor")
                fresh("NOTE saved", lambda: click(480, 664))
                capture("notes")
                fresh("NOTE completed", lambda: click(66, 135))
                fresh("NOTE reopened", lambda: click(66, 135))
                fresh("CLOCK alarm opened", lambda: click(42, 42))
                fresh("CLOCK alarm editor opened", lambda: click(654, 42))
                capture("alarm-editor")
                fresh("CLOCK saved", lambda: click(480, 657))
                capture("alarm")
                event.key(0xFFBE)
                capture("home-reminder")
                click(100, 566)
                wait_for("NETEASE_LOGIN authenticated")
                wait_for("NETEASE_HOME playlists:")
                click(128, 680)
                wait_for("NETEASE_HOME recommendations: 30")
                capture("music-list")
                click(140, 220)
                wait_for("Audio playing:")
                wait_for("] 正在播放")
                capture("music-player")
                assert sound(), "music PCM silent"
                fresh("已暂停", lambda: click(360, 326))
                assert not sound(), "pause did not mute PCM"
                click(360, 326)
                time.sleep(0.4)
                before = log_path.read_text().count("NETEASE_GUEST seek:")
                event(6, 0, 64, 582)
                event(4, 1, 64, 582)
                event(6, 0, 260, 582)
                assert log_path.read_text().count("NETEASE_GUEST seek:") == before
                fresh("NETEASE_GUEST seek:", lambda: event(5, 1, 260, 582))
                fresh("NETEASE_LIKE confirmed: liked=1", lambda: click(128, 480))
                capture(
                    "music-liked",
                    lambda shot: (
                        sum(
                            r > 150 and r > g * 1.5 and r > b * 1.5
                            for r, g, b in shot.crop((96, 448, 168, 512)).getdata()
                        )
                        > 60
                    ),
                )
                escape()
                wait_for("HOME_WIDGET snapshot: app=1 template=3")
                capture("home-music")
                assert sound(), "Home stopped background music"
                fresh("HOME_MEDIA paused=1", lambda: click(207, 601))
                assert not sound()
                fresh("HOME_MEDIA paused=0", lambda: click(207, 601))
                assert sound()
                fresh("HOME catalog opened", lambda: click(540, 570))
                capture("apps")
                fresh("HOME catalog closed", escape)

            origin = f"http://127.0.0.1:{server.server_port}"
            args = [
                "--home",
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
                *paths,
            ]
            navigate_x11(
                env,
                directory,
                args,
                driver,
                seconds=120 if rv32 else 90,
                wait_seconds=30,
            )
            log = (directory / "navigation.log").read_text()
            (results / "ui.log").write_text(log)
            assert (
                "Guest failed:" not in log and TOKEN not in log and CSRF not in log
            ), log[-16000:]
            with Image.open(results / "home.png") as shot:
                assert (
                    sum(
                        r > 180 and g > 180 and b > 180
                        for r, g, b in shot.convert("RGB")
                        .crop((54, 90, 460, 215))
                        .getdata()
                    )
                    > 2000
                ), "large numeric Home clock missing"
            with Image.open(results / "music-player.png") as shot:
                assert (
                    sum(
                        r > 200 and 110 < g < 200 and b < 100
                        for r, g, b in shot.convert("RGB")
                        .crop((296, 262, 424, 390))
                        .getdata()
                    )
                    > 6500
                ), "orange primary play control missing"
            with Image.open(results / "weather.png") as shot:
                assert (
                    sum(
                        r > 180 and g > 180 and b > 180
                        for r, g, b in shot.convert("RGB")
                        .crop((24, 90, 340, 210))
                        .getdata()
                    )
                    > 1500
                ), "large weather temperature missing"
            assert not LoginHandler.failures, LoginHandler.failures

            def restored_city(click, event, wait_for, log_path, process, escape):
                wait_for("WEATHER service ready")
                click(570, 185)
                wait_for("WEATHER activity created")
                click(
                    654, 42
                )  # The durable periodic job can still be scheduled in the future.
                wait_for("WEATHER refresh success")
                query = Handler.weather_queries[-1]
                assert (
                    43 < float(query["latitude"][0]) < 44
                    and 87 < float(query["longitude"][0]) < 88
                ), query

            navigate_x11(
                env,
                directory,
                args,
                restored_city,
                seconds=18 if rv32 else 12,
                wait_seconds=15,
            )
            restart = (directory / "navigation.log").read_text()
            assert "Guest failed:" not in restart, restart[-16000:]
            (results / "city-restored.log").write_text(restart)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    print(
        "PASS panel: numeric Home, 394-city catalog/popular/province/scroll/persist, Material weather icons/forecast/cache, calendar/Notes, durable reminders, music controls/favorites/seek, background PCM and screenshots"
    )


if __name__ == "__main__":
    main()
