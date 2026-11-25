#!/usr/bin/env python3
"""Download, render, scroll and cache actual article text with four live Guests."""

import http.server
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

from PIL import Image
from pc_test_backend import ROOT, app_path
from test_pc_components import descriptor
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler, images
from test_pc_market import navigate_x11, open_home_app, close_home_app
from test_pc_music import mp3
from test_pc_news import Handler as NewsHandler


class Handler(NewsHandler):
    feed_mode = "good"
    article_mode = "good"
    article_delay = 0.2
    article_requests = []

    def do_GET(self):
        if self.path == "/content-feed":
            if Handler.feed_mode == "failure":
                return self.content_reply(b"unavailable", 503)
            rows = [
                {
                    "title": f"第{i + 1}条：新闻正文阅读验证",
                    "url": f"https://news.cctv.com/2026/11/17/ARTIfixture{i + 1:03d}261117.shtml",
                }
                for i in range(50)
            ]
            raw = (
                "news("
                + json.dumps({"data": {"list": rows}}, ensure_ascii=False)
                + ");"
            )
            return self.content_reply(raw.encode())
        if self.path.startswith("/articles/"):
            assert "MicroPixel/1.0" in self.headers.get("User-Agent", "")
            Handler.article_requests.append(self.path)
            mode = Handler.article_mode
            time.sleep(Handler.article_delay)
            if mode == "failure":
                return self.content_reply(b"unavailable", 503)
            if mode == "malformed":
                return self.content_reply(b"<html><nav>no article here</nav></html>")
            markup = "".join(
                f"<p>正文第{i + 1}段：这是一段用于验证新闻实际下载与滚动阅读的内容，不是热榜标题。界面需要保留段落、正确显示中文，并允许读者一直滚动到文章末尾。</p>"
                for i in range(24)
            )
            if mode == "video":
                markup = (
                    "<p>[!--begin:htmlVideoCode--]video-id[!--end:htmlVideoCode--]</p>"
                )
            html = (
                '<html><meta name="source" content="测试新闻源"><script>var contentdate = '
                + json.dumps(markup, ensure_ascii=False)
                + ';</script><div id="content_area"></div><footer>not article text</footer></html>'
            )
            return self.content_reply(html.encode())
        return super().do_GET()

    def content_reply(self, data, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    rv32 = "--rv32" in sys.argv
    live = "--live" in sys.argv
    results = ROOT / (
        "build/news-content-rv32-tests" if rv32 else "build/news-content-tests"
    )
    results.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=results) as directory:
        output = Path(directory)
        if live:
            env = dict(
                os.environ,
                MICROPIXEL_PC_DATA_DIR=str(output / "data"),
                SDL_AUDIODRIVER="dummy",
            )

            def driver(click, event, wait_for, log_path, process, escape):
                wait_for("NEWS refresh success: 50")
                click(150, 220)
                wait_for("NEWS article success: bytes=")
                wait_for("NEWS article layout ready")
                text = log_path.read_text()
                import re

                match = re.search(r"NEWS article success: bytes=(\d+)", text)
                assert match and int(match[1]) > 100, text
                event.key(0xFFC9)
                wait_for("Screenshot:")
                (results / "live-body.png").write_bytes(
                    (output / "navigation.png").read_bytes()
                )

            navigate_x11(
                env,
                output,
                ["--autostart", app_path("news")],
                driver,
                seconds=35 if rv32 else 25,
                wait_seconds=25,
            )
            text = (output / "navigation.log").read_text()
            assert "Guest failed:" not in text, text
            (results / "live.log").write_text(text)
            print(
                "PASS live CCTV list and article HTTPS via Guest SDK: downloaded and rendered article text"
            )
            return
        images()
        LoginHandler.mode = "success"
        HomeHandler.avatar, HomeHandler.audio = LoginHandler.png, mp3(180)
        HomeHandler.resolve_id = 1001
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
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
            args = ["--home", *paths]
            for name, value in [
                ("--test-origin", origin),
                ("--weather-origin", origin + "/weather"),
                ("--news-origin", origin + "/news"),
                ("--news-content-origin", origin + "/content-feed"),
                ("--news-article-origin", origin + "/articles/"),
            ]:
                args += ["--guest-arg", name, "--guest-arg", value]
            args += ["--guest-arg", "--login"]

            def driver(click, event, wait_for, log_path, process, escape):
                def act(action, *markers):
                    at = len(log_path.read_text())
                    action()
                    for marker in markers:
                        end = time.monotonic() + 30
                        while (
                            marker not in log_path.read_text()[at:]
                            and time.monotonic() < end
                        ):
                            time.sleep(0.04)
                        assert marker in log_path.read_text()[at:], (
                            log_path.read_text()[-18000:]
                        )
                    return at

                def capture(name, body=False):
                    end = time.monotonic() + 10
                    while True:
                        act(lambda: event.key(0xFFC9), "Screenshot:")
                        target = results / (name + ".png")
                        target.write_bytes((output / "navigation.png").read_bytes())
                        with Image.open(target) as img:
                            rgb = img.convert("RGB")
                            if (
                                not body
                                or sum(
                                    r > 180 and g > 180 and b > 180
                                    for r, g, b in rgb.crop(
                                        (48, 240, 670, 570)
                                    ).getdata()
                                )
                                > 4000
                            ):
                                return
                        assert time.monotonic() < end, "article body not painted"
                        time.sleep(0.1)

                def sound():
                    start = pcm.stat().st_size
                    time.sleep(0.3)
                    with pcm.open("rb") as f:
                        f.seek(start)
                        return any(f.read())

                def drag(start, end):
                    event(6, 0, 500, start)
                    event(4, 1, 500, start)
                    for i in range(1, 9):
                        event(6, 0, 500, start + (end - start) * i // 8)
                    event(5, 1, 500, end)

                wait_for("WEATHER service ready")
                click(100, 566)
                wait_for("NETEASE_HOME playlists:")
                click(128, 101)
                wait_for("NETEASE_HOME recommendations: 30")
                click(140, 261)
                wait_for("Audio playing:")
                escape()
                act(lambda: open_home_app(click, 2), "NEWS refresh success: 50")
                capture("cctv-list")
                click(150, 42)
                act(
                    lambda: click(360, 288),
                    "NEWS source toutiao",
                    "NEWS refresh success: 50",
                )
                act(lambda: click(150, 220), "NEWS detail opened: 1")
                capture("toutiao-topic")
                assert not Handler.article_requests, (
                    "topic title caused an article request"
                )
                act(
                    lambda: click(360, 666),
                    "NEWS source cctv",
                    "NEWS refresh success: 50",
                )
                act(
                    lambda: click(150, 220),
                    "NEWS article success: bytes=",
                    "NEWS article layout ready",
                )
                capture("article-body", True)
                assert sound(), "article reading stopped music"
                before = (
                    Image.open(results / "article-body.png")
                    .crop((48, 120, 672, 582))
                    .tobytes()
                )
                drag(550, 190)
                capture("article-scrolled", True)
                assert (
                    Image.open(results / "article-scrolled.png")
                    .crop((48, 120, 672, 582))
                    .tobytes()
                    != before
                ), "article did not scroll"
                act(lambda: click(42, 42), "NEWS detail closed")
                Handler.article_mode = "failure"
                at = act(
                    lambda: click(150, 220),
                    "NEWS article failed",
                    "NEWS article layout ready",
                )
                assert "NEWS article cache loaded" in log_path.read_text()[at:]
                capture("article-offline-cache", True)
                Handler.article_mode = "good"
                act(
                    lambda: click(654, 42),
                    "NEWS article success: bytes=",
                    "NEWS article layout ready",
                )
                act(lambda: click(42, 42), "NEWS detail closed")
                Handler.article_mode = "malformed"
                act(lambda: click(150, 400), "NEWS article failed")
                capture("article-error")
                Handler.article_mode = "good"
                act(
                    lambda: click(654, 42),
                    "NEWS article success: bytes=",
                    "NEWS article layout ready",
                )
                act(lambda: click(42, 42), "NEWS detail closed")
                Handler.article_mode = "video"
                act(lambda: click(150, 565), "NEWS article success: bytes=0")
                capture("article-video")
                act(lambda: click(42, 42), "NEWS detail closed")
                Handler.article_mode = "good"
                Handler.article_delay = 5
                drag(600, 215)
                act(lambda: click(150, 350), "NEWS article started")
                act(escape, "NEWS activity stopped")
                assert sound(), "cancelling article stopped music"
                Handler.article_delay = 0
                act(
                    lambda: open_home_app(click, 2),
                    "NEWS activity resumed",
                    "NEWS article success: bytes=",
                    "NEWS article layout ready",
                )
                capture("article-resumed", True)
                act(lambda: click(42, 42), "NEWS detail closed")
                act(
                    lambda: close_home_app(click, escape),
                    "APP_SHELL stopped: micropixel.news",
                )
                Handler.feed_mode = "failure"
                Handler.article_mode = "failure"
                act(
                    lambda: open_home_app(click, 2),
                    "NEWS cache loaded: 12",
                    "NEWS refresh failed",
                )
                drag(600, 215)
                at = act(
                    lambda: click(150, 350),
                    "NEWS article cache loaded",
                    "NEWS article failed",
                    "NEWS article layout ready",
                )
                capture("article-cache-reopened", True)
                assert sound(), "reopening offline article stopped music"
                escape()

            navigate_x11(
                env, output, args, driver, seconds=125 if rv32 else 105, wait_seconds=25
            )
            text = (output / "navigation.log").read_text()
            (results / "ui.log").write_text(text)
            assert (
                "Guest failed:" not in text
                and text.count("NETEASE_LOGIN authenticated") == 1
            ), text[-20000:]
            assert len(Handler.article_requests) >= 8, Handler.article_requests
            assert text.count("APP_SHELL started: micropixel.news") == 2
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    print(
        "PASS article download/render/scroll, sources, retry, malformed/video content, offline body cache, cancel/resume/reopen and four-app PCM"
    )


if __name__ == "__main__":
    main()
