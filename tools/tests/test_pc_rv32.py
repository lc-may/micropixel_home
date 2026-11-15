#!/usr/bin/env python3
"""Execute a pinned public market AOT with QEMU, then test the Host boundary."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time

from PIL import Image
from test_pc_market import navigate_x11

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("MICROPIXEL_PC_BINARY", ROOT / "build/pc/micropixel-pc"))
RELEASE = "1051d7f1-fbcd-42a4-8dd6-03b6e050017a"
DIGEST = "0afebebc14e224646bbd719c7728093fffc99dbce64847375cc92c2203a0d0db"
APP_ID = "com.lckfb.jump"


def run(*args, env, expected=0):
    result = subprocess.run([str(BINARY), *map(str, args)], cwd=ROOT, env=env, capture_output=True, timeout=25)
    log = result.stderr.decode(errors="replace")
    assert result.returncode == expected, f"exit={result.returncode}: {log}"
    return log


class Store(http.server.BaseHTTPRequestHandler):
    bundle = b""
    mode = "good"

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path.endswith("/download"):
            data = self.bundle
        else:
            release = {"id": RELEASE, "target": "riscv32-ilp32f", "version": "0.19.1",
                       "sha256": DIGEST, "sizeBytes": len(self.bundle)}
            if self.mode == "hash":
                release["sha256"] = "0" * 64
            if self.mode == "version":
                release["version"] = "99.0.0"
            if self.mode == "xtensa":
                release["target"] = "xtensa"
            data = json.dumps({"appId": APP_ID, "title": "Jump", "releases": [release]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


FAKE_QEMU = '''#!/usr/bin/env python3
import os,socket,struct,sys,time
args=sys.argv
channel=args[args.index('-chardev')+1]
path=next(p[5:] for p in channel.split(',') if p.startswith('path='))
server=socket.socket(socket.AF_UNIX);server.bind(path);server.listen(1)
sock,_=server.accept()
def frame(kind,seq,base=0,size=0,argv=(),version=1,payload=0,result=0):
 return struct.pack('<20I',0x5652504d,version,kind,seq,base,size,*(list(argv)+[0]*(10-len(argv))),result,0,payload,0)
def reply():
 data=b''
 while len(data)<80:
  part=sock.recv(80-len(data))
  if not part:raise EOFError()
  data+=part
 return struct.unpack('<20I',data)
mode=os.environ['RV32_FAKE_MODE']
sock.sendall(frame(1,1,version=2 if mode=='version' else 1))
if mode=='version':time.sleep(1);sys.exit()
assert reply()[16]==0
if mode=='truncated':sock.sendall(frame(7,2)[:17]);sys.exit()
if mode=='trace':sock.sendall(frame(2,0,payload=513));sys.exit()
sock.sendall(frame(7,2,0x81000000,65536,[1,65536,0x8100fffe,80]))
assert reply()[16]>=0x80000000
sock.sendall(frame(9,3,0x81000000,65536,[1,1,0x81000000,0xffffffff]))
assert reply()[16]>=0x80000000
sock.sendall(frame(10,0,result=0))
'''


def main():
    cache = ROOT / "build/pc-rv32-tests"
    cache.mkdir(parents=True, exist_ok=True)
    bundle = cache / "official-jump.bundle.bin"
    if not bundle.exists():
        previous = ROOT / "build/official-jump-rv32.bundle.bin"
        if previous.exists():
            bundle.write_bytes(previous.read_bytes())
        else:
            run("--http", f"https://micropixel.ai/api/v1/store/releases/{RELEASE}/download",
                "--output", bundle, env=os.environ)
    data = bundle.read_bytes()
    assert len(data) == 655360 and hashlib.sha256(data).hexdigest() == DIGEST
    Store.bundle = data
    with tempfile.TemporaryDirectory(prefix="rv32-tests-", dir=ROOT / "build") as directory:
        output = Path(directory)
        env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(output / "storage"))
        if "--interactive" in sys.argv:
            pcm = output / "audio.pcm"
            env.update(SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(pcm))

            def drive(click, event, wait_for, log_path, process, escape):
                for index in range(3):
                    deadline = time.monotonic() + 4
                    while log_path.read_text(errors="replace").count("first frame up") <= index and time.monotonic() < deadline:
                        time.sleep(0.05)
                    assert log_path.read_text(errors="replace").count("first frame up") == index + 1
                    click(360, 230)
                    event(6, 0, 360, 500)
                    event(4, 1, 360, 500)
                    time.sleep(0.25)
                    event(5, 1, 360, 500)
                    time.sleep(0.3)
                    escape()
                    time.sleep(0.3)
                    if index < 2:
                        click(120, 240)
            navigate_x11(env, output, ["--autostart", bundle], drive, seconds=12)
            log = (output / "navigation.log").read_text(errors="replace")
            assert log.count("RV32 bridge connected") == 3, log
            assert log.count("surface destroyed") == 3, log
            assert pcm.exists() and any(pcm.read_bytes()), "No non-silent PCM after game input"
            (cache / "interaction.log").write_text(log)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Store)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                def install(click, event, wait_for, log_path, process, escape):
                    wait_for("Market detail: com.lckfb.jump")
                    click(120, 390)
                    wait_for("Market installed: com.lckfb.jump")
                    wait_for("first frame up")
                network_env = dict(env, MICROPIXEL_PC_MARKET_URL=f"http://127.0.0.1:{server.server_port}",
                                   SDL_AUDIODRIVER="dummy")
                navigate_x11(network_env, output, ["--market-app", APP_ID], install, seconds=5)
                log = (output / "navigation.log").read_text(errors="replace")
                assert "Market installed:" in log and "RV32 AOT ready" in log, log
                (cache / "market-install.log").write_text(log)
            finally:
                server.shutdown(); server.server_close(); thread.join()
            print("PASS RV32 official AOT touch input, non-silent audio, 3 start/stop cycles and market-button install/run")
            return
        screenshot = output / "jump.png"
        log = run("--headless", "--autostart", "--seconds", 2, "--screenshot", screenshot, bundle, env=env)
        assert "RV32 AOT ready" in log and "first frame up" in log, log
        with Image.open(screenshot) as image:
            pixels = list(image.convert("RGB").getdata())
            assert sum(g > r * 1.2 and g > b * 1.2 for r, g, b in pixels) > 3000, "Game tile was not rendered"
        (cache / "execution.log").write_text(log)
        (cache / "official-jump.png").write_bytes(screenshot.read_bytes())
        header = struct.unpack_from("<8sIIII64s10I", data)
        aot_slot = next(header[4] + i * 48 for i in range(header[7]) if struct.unpack_from("<I", data, header[4] + i * 48)[0] == 1)
        aot_offset = struct.unpack_from("<I", data, aot_slot + 8)[0]
        bad = bytearray(data)
        bad[aot_offset + 100] ^= 1
        broken = output / "broken.bundle.bin"
        broken.write_bytes(bad)
        log = run("--headless", "--autostart", "--seconds", .1, broken, env=env, expected=1)
        assert "Invalid app Bundle" in log, log
        bad = bytearray(data)
        struct.pack_into("<I", bad, aot_slot + 40, 2)
        broken.write_bytes(bad)
        log = run("--headless", "--autostart", "--seconds", .1, broken, env=env, expected=1)
        assert "Xtensa is unsupported" in log, log
        broken.write_bytes(b"MPXBNDL\0" + bytes(8 * 1024 * 1024))
        log = run("--headless", "--autostart", "--seconds", .1, broken, env=env, expected=1)
        assert "exceeds 8 MiB" in log, log
        fake = output / "fake-qemu"
        fake.write_text(FAKE_QEMU)
        fake.chmod(0o700)
        for mode in ("pointers", "version", "truncated", "trace"):
            log = run("--headless", "--autostart", "--exit-on-app-complete", "--seconds", 3, bundle,
                      env=dict(env, MICROPIXEL_QEMU_RV32=str(fake), RV32_FAKE_MODE=mode),
                      expected=0 if mode == "pointers" else 1)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Store)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            network_env = dict(env, MICROPIXEL_PC_MARKET_URL=f"http://127.0.0.1:{server.server_port}")
            run("--headless", "--install-app", APP_ID, "--seconds", .5, env=network_env)
            installed = Path(env["MICROPIXEL_PC_DATA_DIR"]) / "apps" / f"{APP_ID}.bundle.bin"
            assert hashlib.sha256(installed.read_bytes()).hexdigest() == DIGEST
            for mode, marker in [("hash", "SHA-256 mismatch"), ("version", "identity/version mismatch"),
                                 ("xtensa", "no available riscv32")]:
                Store.mode = mode
                log = run("--headless", "--install-app", APP_ID, "--seconds", .1, env=network_env, expected=1)
                assert marker in log, log
                assert hashlib.sha256(installed.read_bytes()).hexdigest() == DIGEST, "Failed update changed old app"
            assert not list(installed.parent.glob(".install-*")), "Staging files leaked"
        finally:
            server.shutdown(); server.server_close(); thread.join()
    print("PASS RV32 official AOT rendering, Bundle hashes/architecture, IPC bounds/framing and transactional market install")


if __name__ == "__main__":
    main()
