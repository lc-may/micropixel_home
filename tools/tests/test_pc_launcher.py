#!/usr/bin/env python3
"""Host Home, input isolation, live background music and single-Guest replacement."""
from test_pc_market import open_home_app, open_home_market
import http.server
import io
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

from PIL import Image
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler, images, ROOT, TOKEN, CSRF
from test_pc_market import Handler as MarketHandler, navigate_x11
from test_pc_music import mp3


def main():
    images()
    LoginHandler.mode = "success"
    MarketHandler.mode = "good"
    picture = io.BytesIO()
    Image.new("RGB", (96, 96), (205, 65, 83)).save(picture, "PNG")
    HomeHandler.avatar, HomeHandler.audio = picture.getvalue(), mp3(3)
    longer_audio = mp3(12)
    servers = [http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
               for handler in (HomeHandler, MarketHandler)]
    threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in servers]
    for thread in threads:
        thread.start()
    rv32 = "--rv32" in sys.argv
    music = ROOT / ("build/pc-netease-release/netease-music.bundle.bin" if rv32
                    else "build/pc-apps/netease-music/netease-music.pc.json")
    other = ROOT / "build/pc-apps/timer_counter/timer_counter.pc.json"
    ordered = sorted([str(music), str(other)])
    other_x = 190 if ordered.index(str(other)) == 0 else 530
    music_x = 190 if ordered.index(str(music)) == 0 else 530
    results = ROOT / "build/pc-launcher-tests"
    results.mkdir(exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="pc-launcher-", dir=ROOT / "build") as directory:
            output = Path(directory)
            pcm = output / "background.pcm"
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(output / "data"), SDL_AUDIODRIVER="disk",
                       SDL_DISKAUDIOFILE=str(pcm),
                       MICROPIXEL_PC_MARKET_URL=f"http://127.0.0.1:{servers[1].server_port}")
            args = ["--home", "--single-app", str(music), str(other), "--guest-arg", "--login",
                    "--guest-arg", "--test-origin", "--guest-arg", f"http://127.0.0.1:{servers[0].server_port}"]

            def scenario(name, driver, seconds):
                navigate_x11(env, output, args, driver, seconds=seconds, wait_seconds=10 if rv32 else 5)
                log = (output / "navigation.log").read_text()
                assert TOKEN not in log and CSRF not in log
                assert "Guest failed:" not in log and "Guest initialization failed:" not in log, log
                with Image.open(output / "navigation.png") as screenshot:
                    assert screenshot.convert("RGB").getpixel((4, 4)) == (21, 24, 29), "Home was not above the Guest"
                (results / f"{name}.log").write_text(log)
                (results / f"{name}.png").write_bytes((output / "navigation.png").read_bytes())

            def helpers(event, log_path):
                def wait(marker, action=lambda: None):
                    offset = len(log_path.read_text())
                    action()
                    deadline = time.monotonic() + (12 if rv32 else 6)
                    while marker not in log_path.read_text()[offset:] and time.monotonic() < deadline:
                        time.sleep(.03)
                    assert marker in log_path.read_text()[offset:], log_path.read_text()
                def home():
                    event.key(0xffbe)  # F1, intercepted by Host before Guest input.
                def samples():
                    time.sleep(.12)
                    start = pcm.stat().st_size
                    time.sleep(.3)
                    chunk = pcm.read_bytes()[start:]
                    assert len(chunk) > 8192, "audio device stopped producing PCM"
                    return chunk
                return wait, home, samples

            def background(click, event, wait_for, log_path, process, escape):
                wait, home, samples = helpers(event, log_path)
                wait_for("HOME visible: running=none apps=2")
                click(296, 686)
                wait_for("NETEASE_HOME avatar ready")
                wait("NETEASE_HOME recommendations: 30", lambda: click(128, 680))
                HomeHandler.resolve_id = 1001
                wait("Audio playing:", lambda: click(240, 229))
                HomeHandler.audio = longer_audio
                HomeHandler.resolve_id = 1002
                wait("APP_SHELL background: micropixel.netease-music", home)
                seek_count = log_path.read_text().count("NETEASE_GUEST seek:")
                click(650, 500)  # Empty Home space over the Guest seek slider.
                assert log_path.read_text().count("NETEASE_GUEST seek:") == seek_count, "Home input reached Guest"
                wait_for("NETEASE_GUEST selected: 1002")  # Guest timer/network/queue continue under Home.
                deadline = time.monotonic() + 5
                while log_path.read_text().count("Audio playing:") < 2 and time.monotonic() < deadline:
                    time.sleep(.03)
                assert log_path.read_text().count("Audio playing:") >= 2
                assert any(samples()), "background playback was silent"
                wait("HOME_MEDIA paused=1", lambda: click(548, 606))
                assert not any(samples()), "Home pause did not silence playback"
                HomeHandler.resolve_id = 1003
                wait("HOME_MEDIA paused=0", lambda: click(548, 606))
                assert any(samples()), "Home resume did not restore playback"
                open_home_market(click)
                wait_for("Market catalog: 7 entries")
                assert "APP_SHELL stopped:" not in log_path.read_text(), "market closed background Guest"
                wait("HOME visible: running=micropixel.netease-music", escape)
                wait("APP_SHELL foreground: micropixel.netease-music reused=1", lambda: click(200, 606))
                wait("APP_SHELL background:", home)
                wait("APP_SHELL foreground: micropixel.netease-music reused=1", lambda: open_home_app(click, 0 if music_x < 300 else 1))
                assert log_path.read_text().count("NETEASE_GUEST started:") == 1, "music Guest was recreated"
                assert log_path.read_text().count("NETEASE_LOGIN authenticated") == 1, "login was lost"
                wait("APP_SHELL background:", home)
                wait("APP_SHELL started: micropixel.pc.timer_counter", lambda: open_home_app(click, 0 if other_x < 300 else 1))
                wait_for("timer_counter: terminal Cancel, missed_count, Clock/TimePoint and RAII passed")
                wait_for("HOME visible: running=none apps=2")
                log = log_path.read_text()
                assert log.index("APP_SHELL stopped: micropixel.netease-music") < log.index("APP_SHELL started: micropixel.pc.timer_counter")
                assert not any(samples()), "replacing music left its audio running"
            scenario("background", background, 40 if rv32 else 32)

            HomeHandler.resolve_id = 1001
            def close(click, event, wait_for, log_path, process, escape):
                wait, home, samples = helpers(event, log_path)
                wait_for("HOME visible: running=none apps=2")
                click(296, 686)
                wait_for("NETEASE_HOME avatar ready")
                wait("APP_SHELL background:", home)
                wait("APP_SHELL stopped: micropixel.netease-music", lambda: click(650, 692))
                wait_for("HOME visible: running=none apps=2")
            scenario("close", close, 12 if rv32 else 9)
            assert not LoginHandler.failures, LoginHandler.failures
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join()
    print("PASS Host Home launch/close, background stereo audio and automatic next track, input isolation, market coexistence, same-session resume and ordered Guest replacement")


if __name__ == "__main__":
    main()
