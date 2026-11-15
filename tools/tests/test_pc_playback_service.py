#!/usr/bin/env python3
"""D4: playback survives a long page request, another Guest and UI teardown."""
import array
import http.server
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import urllib.parse

from test_pc_components import descriptor
from test_pc_login import Handler as LoginHandler, images
from test_pc_market import navigate_x11, ROOT
from test_pc_music import mp3
from test_pc_home import HomeHandler


class Handler(LoginHandler):
    audio = b""
    resolve_count = 0
    long_started = threading.Event()
    long_done = threading.Event()
    resolved_during_poll = 0

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/tone.mp3":
            return self.reply(self.audio)
        if path.startswith(("/__poll/", "/__poll-new/")):
            self.long_started.set()
            time.sleep(35)
            self.long_done.set()
            return self.reply(b"window.wx_errcode=408;window.wx_code='';")
        super().do_GET()

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        if path in ("/weapi/search/get", "/weapi/song/enhance/player/url", "/weapi/v3/song/detail"):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            tracks = [{"id": 101 + i, "name": f"Fixture {i}", "ar": [{"name": "PCM"}],
                       "al": {"picUrl": "https://p1.music.126.net/fixture.jpg"}, "dt": 3000} for i in range(6)]
            if path.endswith("search/get"):
                result = {"code": 200, "result": {"songCount": len(tracks), "songs": tracks}}
            elif path.endswith("song/detail"):
                result = {"code": 200, "songs": [tracks[min(self.resolve_count, 5)]]}
            else:
                Handler.resolve_count += 1
                if self.long_started.is_set() and not self.long_done.is_set():
                    Handler.resolved_during_poll += 1
                result = {"code": 200, "data": [{"id": 100 + self.resolve_count,
                           "url": f"http://127.0.0.1:{self.server.server_port}/tone.mp3"}]}
            return self.reply(json.dumps(result).encode())
        super().do_POST()


def main():
    images()
    Handler.audio = mp3(3)
    LoginHandler.mode = "wait"
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = ROOT / "build/pc-playback-service-tests"
    results.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(dir=results) as temporary:
            directory = Path(temporary)
            music = descriptor(ROOT / "build/pc-apps/netease-music/netease-music.pc.json",
                               directory / "a-music.pc.json", "micropixel.netease-music")
            peer = descriptor(ROOT / "build/pc-apps/components-demo/components-demo.pc.json",
                              directory / "b-peer.pc.json", "test.playback.peer")
            pcm = results / "background.pcm"
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "storage"),
                       SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(pcm))

            def scenario(click, event, wait_for, log_path, process, escape):
                click(120, 180)
                wait_for("NETEASE_GUEST search: 6 tracks")
                click(130, 190)
                wait_for("NETEASE_GUEST selected: 101")
                wait_for("Audio playing:")
                click(48, 38)  # Player -> browser, whose account button starts QR login.
                click(650, 38)
                wait_for("NETEASE_LOGIN poll start:")
                assert Handler.long_started.wait(3), log_path.read_text()
                event.key(0xffbe)
                click(480, 180)
                wait_for("APP_SHELL started: test.playback.peer")
                wait_for("NETEASE_GUEST selected: 102")
                wait_for("NETEASE_GUEST selected: 103")
                assert Handler.resolved_during_poll >= 2 and not Handler.long_done.is_set()
                assert log_path.read_text().count("NETEASE_GUEST started:") == 1
                event.key(0xff0d)
                wait_for("COMP key received")
                # Keep both Guests alive until the real 35-second request expires.

            args = ["--home", "--multi-app", "--guest-arg", "--test-origin", "--guest-arg",
                    f"http://127.0.0.1:{server.server_port}", music, peer]
            navigate_x11(env, results, args, driver=scenario, seconds=39, wait_seconds=6)
            assert Handler.long_done.wait(3)
            log = (results / "navigation.log").read_text()
            assert "Guest failed:" not in log, log
            (results / "long-poll.log").write_text(log)
            assert any(array.array("h", pcm.read_bytes())), "silent PCM"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    # Logged-in model survives destruction and recreation of its Activity.
    LoginHandler.mode = "success"
    HomeHandler.resolve_id = 1001
    HomeHandler.audio, HomeHandler.avatar = mp3(12), LoginHandler.png
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HomeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(dir=results) as temporary:
            directory = Path(temporary)
            music = descriptor(ROOT / "build/pc-apps/netease-music/netease-music.pc.json",
                               directory / "a-music.pc.json", "micropixel.netease-music")
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(directory / "storage"))

            def recreate(click, event, wait_for, log_path, process, escape):
                click(120, 180)
                wait_for("NETEASE_LOGIN authenticated")
                wait_for("NETEASE_HOME playlists:")
                click(120, 680)
                wait_for("NETEASE_HOME recommendations: 30")
                click(140, 220)
                wait_for("NETEASE_ACTIVITY destroyed")
                wait_for("Audio playing:")
                before = log_path.read_text().count("NETEASE_ACTIVITY created")
                click(120, 180)
                deadline = time.monotonic() + 4
                while log_path.read_text().count("NETEASE_ACTIVITY created") == before and time.monotonic() < deadline:
                    time.sleep(.03)
                log = log_path.read_text()
                assert log.count("NETEASE_ACTIVITY created") == 2, log
                assert log.count("NETEASE_SERVICE started") == 1 and log.count("NETEASE_LOGIN authenticated") == 1, log
                assert log.count("NETEASE_GUEST started:") == 1 and log.count("Audio playing:") == 1, log
                assert "NETEASE_ACTIVITY resumed" in log

            args = ["--home", "--multi-app", "--guest-arg", "--login", "--guest-arg", "--destroy-activity",
                    "--guest-arg", "--test-origin", "--guest-arg", f"http://127.0.0.1:{server.server_port}", music]
            navigate_x11(env, results, args, driver=recreate, seconds=12, wait_seconds=6)
            log = (results / "navigation.log").read_text()
            assert "Guest failed:" not in log, log
            (results / "recreate.log").write_text(log)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS music Service: 35-second page poll, background auto-next, foreground peer, account/queue/media retained after Activity recreation")


if __name__ == "__main__":
    main()
