#!/usr/bin/env python3
"""Guest SDK weapi/MP3/player integration; fixture traffic stays on loopback."""
from array import array
import base64
import http.server
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import wave

from PIL import Image
from test_pc_market import close_home_app, navigate_x11

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("MICROPIXEL_PC_BINARY", ROOT / "build/pc/micropixel-pc"))


def mp3(seconds):
    out = io.BytesIO()
    with wave.open(out, "wb") as file:
        file.setnchannels(2); file.setsampwidth(2); file.setframerate(44100)
        samples = bytearray()
        for i in range(int(seconds * 44100)):
            samples.extend(struct.pack("<hh", int(10000 * math.sin(2 * math.pi * 440 * i / 44100)),
                                       int(10000 * math.sin(2 * math.pi * 660 * i / 44100))))
        file.writeframes(samples)
    encoder = shutil.which("ffmpeg")
    assert encoder, "Music tests require ffmpeg to generate a synthetic stereo MP3 fixture"
    return subprocess.run([encoder, "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-c:a", "libmp3lame",
                           "-b:a", "128k", "-f", "mp3", "pipe:1"], input=out.getvalue(), capture_output=True,
                          check=True).stdout


class Handler(http.server.BaseHTTPRequestHandler):
    mode = "full"
    audio = b""
    short_audio = b""
    hits = []
    failures = []
    media_waiting = threading.Event()
    release_media = threading.Event()

    def log_message(self, *_):
        pass

    def reply(self, data, status=200):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        Handler.hits.append(self.path)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            assert 0 < length <= 128 * 1024
            body = urllib.parse.parse_qs(self.rfile.read(length).decode())
            assert len(base64.b64decode(body["params"][0], validate=True)) % 16 == 0
            assert re.fullmatch("[a-f0-9]{256}", body["encSecKey"][0])
            assert "MUSIC_U" not in self.headers.get("Cookie", "")
            assert not self.headers.get("X-Forwarded-For")
            assert self.headers.get("Accept-Encoding") == "identity"
            assert self.headers.get("Referer") == "https://music.163.com/"
        except Exception as error:
            Handler.failures.append(repr(error)); self.reply(b"Bad request", 400); return
        mode = Handler.mode
        if mode == "badjson":
            self.reply(b"not JSON"); return
        if mode == "restricted":
            self.reply(json.dumps({"code": -460, "message": "访问受限"}).encode()); return
        if mode == "redirect":
            self.send_response(302); self.send_header("Location", "/leaked"); self.send_header("Content-Length", "0"); self.end_headers(); return
        track = {"id": 101, "name": "双声道测试", "artists": [{"name": "Fixture"}], "album": {"name": "Test"},
                 "duration": 8000, "fee": 0}
        if self.path == "/weapi/search/get":
            result = {"code": 200, "result": {"songCount": 0}} if mode == "empty" else {
                "code": 200, "result": {"songCount": 2, "songs": [track, dict(track, id=102, name="第二首")]}}
        elif self.path == "/weapi/v3/song/detail":
            result = {"code": 200, "songs": [track]}
        elif self.path == "/weapi/song/enhance/player/url":
            url = f"http://127.0.0.1:{self.server.server_port}/{'trial' if mode == 'trial' else 'tone'}.mp3"
            if mode == "denied": url = None
            if mode == "external": url = "https://example.com/unapproved.mp3"
            result = {"code": 200, "data": [{"id": 101, "url": url, "type": "mp3",
                "freeTrialInfo": {"start": 0, "end": 3} if mode == "trial" else None}]}
        else:
            self.reply(b"Unknown", 404); return
        self.reply(json.dumps(result, ensure_ascii=False).encode())

    def do_GET(self):
        Handler.hits.append(self.path)
        if self.path == "/probe":
            self.reply(b"0123456789" * 600); return
        if Handler.mode == "slowmedia":
            Handler.media_waiting.set()
            Handler.release_media.wait(5)
        self.reply(self.short_audio if self.path == "/trial.mp3" else self.audio)


def main():
    Handler.audio, Handler.short_audio = mp3(8), mp3(3)
    rv32_bundle = ROOT / "build/pc-rv32-apps/netease-music/netease-music.bundle.bin"
    rv32_args = [str(rv32_bundle)] if "--rv32" in sys.argv else []
    if rv32_args:
        assert rv32_bundle.is_file(), "Build the netease-music release RV32 Bundle first"
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    cache = ROOT / "build/pc-music-tests"; cache.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="pc-music-", dir=ROOT / "build") as directory:
            output = Path(directory)
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(output / "storage"),
                       MICROPIXEL_NETEASE_TEST_URL=f"http://127.0.0.1:{server.server_port}",
                       HTTP_PROXY="http://127.0.0.1:1", HTTPS_PROXY="http://127.0.0.1:1")

            guest_args=["--guest-arg", "--test-origin", "--guest-arg", env["MICROPIXEL_NETEASE_TEST_URL"]]
            def run(*args, expected=0, name="test", disk=False):
                if any(arg in args for arg in ("--music", "--music-query", "--music-song")):
                    args=(*args,*guest_args,*rv32_args)
                    if rv32_args and "--seconds" in args:
                        # QEMU cold startup and Guest RSA need more time than native Wasm.
                        args=list(args)
                        duration=args.index("--seconds")+1
                        args[duration]=str(max(6, float(args[duration])))
                child_env = dict(env)
                if disk: child_env.update(SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(output / f"{name}.pcm"))
                result = subprocess.run([str(BINARY), *map(str, args)], cwd=ROOT, env=child_env,
                                        capture_output=True, timeout=15)
                log = result.stderr.decode(errors="replace")
                assert result.returncode == expected, log
                (cache / f"{name}.log").write_text(log)
                return result.stdout, log

            if "--interactive" in sys.argv:
                pcm = output / "interactive.pcm"

                def drive(click, event, wait_for, log_path, process, escape):
                    wait_for("Audio playing:")
                    click(360, 326); wait_for("已暂停")
                    time.sleep(.12); start = pcm.stat().st_size; time.sleep(.3)
                    paused = pcm.read_bytes()[start:]
                    assert len(paused) > 8192 and not any(paused), "Pause did not stop the audio source"
                    click(360, 326); time.sleep(.2)
                    event(6,0,64,582);event(4,1,64,582);event(6,0,360,582);event(5,1,360,582);wait_for("NETEASE_GUEST seek:")
                    click(650, 40); wait_for("已停止")
                    click(360, 326)
                    deadline = time.monotonic() + 3
                    while log_path.read_text(errors="replace").count("Audio playing:") < 2 and time.monotonic() < deadline:
                        time.sleep(.05)
                    assert log_path.read_text(errors="replace").count("Audio playing:") >= 2
                    close_home_app(click, escape); time.sleep(.3); click(296, 686); wait_for("NETEASE_GUEST search: 2 tracks")
                    before=Handler.hits.count("/weapi/search/get")
                    click(200, 112)
                    for char in "jazz": event.key(ord(char))
                    event.key(0xff08)  # Backspace removes one UTF-8 codepoint.
                    event.key(0xff0d)  # Return submits text through SDK Input 1.2.
                    deadline=time.monotonic()+2
                    while Handler.hits.count("/weapi/search/get")==before and time.monotonic()<deadline: time.sleep(.05)
                    assert Handler.hits.count("/weapi/search/get")>before, "Guest text input did not submit a search"
                    escape()
                navigate_x11(dict(env, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(pcm)), output,
                             ["--music-song", "101", *guest_args, *rv32_args], drive, seconds=18)
                (cache / "interaction.log").write_text((output / "navigation.log").read_text())
                assert any(pcm.read_bytes()), "No actual PCM output"
                Handler.mode = "slowmedia"

                def cancel(click, event, wait_for, log_path, process, escape):
                    wait_for("NETEASE_GUEST selected:")
                    assert Handler.media_waiting.wait(2), "Media request did not start"
                    started = time.monotonic(); click(650, 40); escape()
                    assert time.monotonic() - started < .9, "UI blocked while cancelling the download"
                    Handler.release_media.set()
                navigate_x11(env, output, ["--music-song", "101", *guest_args, *rv32_args], cancel, seconds=6)
                assert not Handler.failures, Handler.failures
                print("PASS music X11 pause/resume, seek, replay, hall navigation and nonblocking download cancellation")
                return
            for mode, marker in [("denied", "不可播放"), ("external", "拒绝非网易云"),
                                 ("badjson", "无效 JSON"), ("restricted", "-460"), ("redirect", "HTTP status 302")]:
                Handler.mode = mode
                _, log = run("--headless", "--music-song", 101, "--seconds", 2, name=mode)
                assert marker in log and "Audio playing:" not in log, log
            assert "/leaked" not in Handler.hits, "POST body followed a redirect"
            Handler.mode = "empty"
            _, log = run("--headless", "--music", "--seconds", 2, name="empty")
            assert "NETEASE_GUEST search: 0 tracks" in log, log
            Handler.mode = "full"
            _, conformance=run("--headless", "--autostart", "--exit-on-app-complete", "--seconds", 12,
                "build/pc-apps/network_media/network_media.pc.json", *guest_args, name="network-media")
            assert "NETWORK_MEDIA PASS" in conformance and "Guest completed: result=0" in conformance, conformance
            _, log = run("--headless", "--music", "--seconds", 2, "--screenshot", output / "search.png", name="search-ui")
            assert "NETEASE_GUEST search: 2 tracks" in log, log
            _, log = run("--headless", "--music-song", 101, "--seconds", 3, "--screenshot", output / "player.png", name="player", disk=True)
            assert "trial=0" in log and "Audio playing:" in log, log
            samples = array("h"); samples.frombytes((output / "player.pcm").read_bytes())
            assert sum(abs(x) > 100 for x in samples) > 5000, "MP3 was silent"
            assert sum(abs(samples[i] - samples[i + 1]) > 100 for i in range(0, len(samples)-1, 2)) > 1000, "Stereo channels were collapsed"
            for name in ("search", "player"):
                with Image.open(output / f"{name}.png") as image: assert image.size == (720, 720)
                shutil.copyfile(output / f"{name}.png", cache / f"{name}.png")
            Handler.mode = "trial"
            _, log = run("--headless", "--music-song", 101, "--seconds", 2, name="trial-ui")
            assert "trial=1" in log and "Audio playing:" in log, log
            Handler.mode = "denied"
            _, log = run("--headless", "--music-song", 101, "--seconds", 2, name="denied-ui")
            assert "不可播放" in log and "Audio playing:" not in log, log
            assert not Handler.failures, Handler.failures
    finally:
        server.shutdown(); server.server_close(); thread.join()
    print("PASS music Guest SDK/wire bounds, AES/RSA/JSON, weapi POST, anonymous search, full/trial/denied states, source/redirect checks and non-silent stereo MP3")


if __name__ == "__main__":
    main()
