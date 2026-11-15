#!/usr/bin/env python3
"""Local integration checks for the actual PC executable and network/audio backends."""
import contextlib
import http.server
import io
import math
import os
import shutil
from pathlib import Path
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("MICROPIXEL_PC_BINARY", ROOT / "build/pc/micropixel-pc"))


def run(*arguments, env=None, success=True):
    result = subprocess.run([str(BINARY), *map(str, arguments)], capture_output=True, timeout=25, env=env)
    if (result.returncode == 0) != success:
        raise AssertionError(f"{arguments}: exit {result.returncode}\n{result.stderr.decode(errors='replace')}")
    return result


def make_wave():
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 16000))) for i in range(3200)))
    return output.getvalue()


AUDIO = make_wave()
OGG = b""


class Http(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/delayed":
            time.sleep(0.2)
        if self.path == "/delayed-chunks":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for chunk in (b"5\r\nhello\r\n", b"6\r\n world\r\n", b"0\r\n\r\n"):
                time.sleep(0.1)
                self.wfile.write(chunk)
                self.wfile.flush()
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/hello")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
            return
        content = self.headers.get("Host", "").encode() if self.path == "/authority" else AUDIO if self.path == "/audio.wav" else OGG if self.path == "/audio.ogg" else b"hello world"
        self.send_response(200)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def exact(stream, count):
    data = b""
    while len(data) < count:
        part = stream.read(count - len(data))
        if not part:
            raise EOFError
        data += part
    return data


class Broker(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(10)
        try:
            while True:
                first = exact(self.rfile, 1)[0]
                length, multiplier = 0, 1
                for _ in range(4):
                    byte = exact(self.rfile, 1)[0]
                    length += (byte & 127) * multiplier
                    if not byte & 128:
                        break
                    multiplier *= 128
                if length > 8192:
                    return
                body = exact(self.rfile, length)
                if first >> 4 == 1:
                    self.wfile.write(b"\x20\x02\0\0")
                elif first >> 4 == 8:
                    self.wfile.write(b"\x90\x03" + body[:2] + b"\0")
                elif first >> 4 == 3:
                    assert length < 128  # Small conformance payload fits a one-byte length.
                    self.wfile.write(bytes([0x30, length]) + body)
                elif first >> 4 == 12:
                    self.wfile.write(b"\xd0\0")
                else:
                    return
                self.wfile.flush()
        except (EOFError, OSError):
            return


@contextlib.contextmanager
def serve(handler, tls=None, port=0):
    server = Server(("127.0.0.1", port), handler)
    if tls:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def main():
    global OGG
    with tempfile.TemporaryDirectory(prefix="pc-network-", dir=ROOT / "build") as directory:
        output = Path(directory)
        local_wave = output / "voice.wav"
        local_wave.write_bytes(AUDIO)
        # Capture the SDL audio callback output to verify non-silent decoded PCM.
        audio_env = dict(os.environ, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(output / "audio.raw"),
                         MICROPIXEL_PC_DATA_DIR=str(output / "storage"))
        run("--headless", "--play", local_wave, "--exit-on-audio-complete", "--seconds", "5", env=audio_env)
        captured = (output / "audio.raw").read_bytes()
        assert captured and any(captured), "WAV playback produced silence"
        if shutil.which("ffmpeg"):
            clip = output / "voice.ogg"
            subprocess.run(["ffmpeg", "-v", "error", "-i", str(local_wave), "-c:a", "libopus", str(clip)], check=True)
            OGG = clip.read_bytes()
            run("--headless", "--play", clip, "--exit-on-audio-complete", "--seconds", "5", env=audio_env)
            assert any((output / "audio.raw").read_bytes()), "Opus playback produced silence"
        with serve(Http) as port:
            base = f"http://127.0.0.1:{port}"
            assert run("--http", base + "/authority").stdout == f"127.0.0.1:{port}".encode()
            for path in ("/hello", "/redirect", "/chunked", "/delayed", "/delayed-chunks"):
                assert run("--http", base + path).stdout == b"hello world"
            result = run("--headless", "--play", base + "/audio.wav", "--exit-on-audio-complete", "--seconds", "5")
            assert b"Audio playing:" in result.stderr
            if OGG:
                result = run("--headless", "--play", base + "/audio.ogg", "--exit-on-audio-complete", "--seconds", "5")
                assert b"Audio playing:" in result.stderr
        with serve(Broker) as port:
            assert b"MQTT received: pc-test" in run("--mqtt", f"mqtt://127.0.0.1:{port}", "--message", "pc-test").stdout
        cert, key = output / "ca.pem", output / "server.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key),
                        "-out", str(cert), "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, capture_output=True)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        trusted_env = dict(os.environ, MICROPIXEL_CA_FILE=str(cert))
        with serve(Http, tls) as port:
            assert run("--http", f"https://localhost:{port}/authority", env=trusted_env).stdout == f"localhost:{port}".encode()
            assert run("--http", f"https://localhost:{port}/hello", env=trusted_env).stdout == b"hello world"
            for path in ("/delayed", "/delayed-chunks"):
                assert run("--http", f"https://localhost:{port}{path}", env=trusted_env).stdout == b"hello world"
            run("--http", f"https://localhost:{port}/hello", success=False)
            run("--http", f"https://127.0.0.1:{port}/hello", env=trusted_env, success=False)
        with serve(Broker, tls) as port:
            assert b"MQTT received: tls-test" in run("--mqtt", f"mqtts://localhost:{port}", "--message", "tls-test", env=trusted_env).stdout
        if "--default-ports" in sys.argv:
            with serve(Http, port=80):
                for authority in ("127.0.0.1", "127.0.0.1:80"):
                    assert run("--http", f"http://{authority}/authority").stdout == b"127.0.0.1"
            with serve(Http, tls, port=443):
                for authority in ("localhost", "localhost:443"):
                    assert run("--http", f"https://{authority}/authority", env=trusted_env).stdout == b"localhost"
            print("PASS HTTP/HTTPS Host normalization on real default ports 80/443")
        for url in ("file:///etc/passwd", "https://user:password@localhost/", "http://localhost:99999/"):
            run("--http", url, success=False)
    print("PASS PC HTTP including delayed headers/chunks, redirects, HTTPS trust/hostname checks, MQTT/MQTTS, local/network WAV/Opus and non-silent SDL output")


if __name__ == "__main__":
    main()
