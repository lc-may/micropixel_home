#!/usr/bin/env python3
"""Exercise the LVGL market with the production executable and local HTTP fixtures."""
import copy
import http.server
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("MICROPIXEL_PC_BINARY", ROOT / "build/pc/micropixel-pc"))
APP = {
    "appId": "test.market.demo", "title": "市场测试 Demo", "summary": "中文说明与在线封面",
    "description": "从官方协议读取的应用详情。", "developer": "PC fixture", "category": "测试",
    "cover": "/cover.png", "releases": [
        {"version": "1.0.0", "target": "riscv32-ilp32f", "sizeBytes": 4096},
        {"version": "1.0.0", "target": "xtensa", "sizeBytes": 8192},
        {"version": "0.9.0", "target": "xtensa", "withdrawn": True},
    ],
}
cover = io.BytesIO()
Image.new("RGB", (80, 80), (220, 30, 150)).save(cover, "PNG")
COVER = cover.getvalue()


def navigate_x11(env, output, arguments=(), driver=None, seconds=10, wait_seconds=3):
    """Send events only to this test's X11 window; do not move the user's pointer."""
    import ctypes as c
    import ctypes.util

    x = c.CDLL(ctypes.util.find_library("X11"))
    pointer, window = c.c_void_p, c.c_ulong

    def bind(name, result, *args):
        function = getattr(x, name)
        function.restype, function.argtypes = result, args
        return function

    open_display = bind("XOpenDisplay", pointer, c.c_char_p)
    root_window = bind("XDefaultRootWindow", window, pointer)
    atom = bind("XInternAtom", window, pointer, c.c_char_p, c.c_int)
    query = bind("XQueryTree", c.c_int, pointer, window, c.POINTER(window), c.POINTER(window),
                 c.POINTER(c.POINTER(window)), c.POINTER(c.c_uint))
    property_get = bind("XGetWindowProperty", c.c_int, pointer, window, window, c.c_long, c.c_long,
                        c.c_int, window, c.POINTER(window), c.POINTER(c.c_int), c.POINTER(window),
                        c.POINTER(window), c.POINTER(pointer))
    free = bind("XFree", c.c_int, pointer)
    fetch_name = bind("XFetchName", c.c_int, pointer, window, c.POINTER(pointer))
    flush = bind("XFlush", c.c_int, pointer)
    close = bind("XCloseDisplay", c.c_int, pointer)
    keycode = bind("XKeysymToKeycode", c.c_uint, pointer, window)
    set_error = bind("XSetErrorHandler", pointer, pointer)
    class XError(c.Structure):
        _fields_=[("type",c.c_int),("display",pointer),("resource",window),("serial",window),
                  ("code",c.c_ubyte),("request",c.c_ubyte),("minor",c.c_ubyte)]
    x_errors=[]
    @c.CFUNCTYPE(c.c_int,pointer,c.POINTER(XError))
    def handle_error(display,error):
        # Other applications may destroy windows during recursive enumeration.
        if error.contents.code!=3:  # BadWindow is retried by find().
            x_errors.append((error.contents.code,error.contents.request,error.contents.minor))
        return 0

    class Event(c.Structure):
        _fields_ = [("type", c.c_int), ("serial", window), ("send_event", c.c_int),
                    ("display", pointer), ("window", window), ("root", window), ("subwindow", window),
                    ("time", window), ("x", c.c_int), ("y", c.c_int), ("x_root", c.c_int),
                    ("y_root", c.c_int), ("state", c.c_uint), ("button", c.c_uint), ("same_screen", c.c_int)]

    class AnyEvent(c.Union):
        _fields_ = [("event", Event), ("padding", c.c_long * 24)]

    send = bind("XSendEvent", c.c_int, pointer, window, c.c_int, c.c_long, c.POINTER(AnyEvent))
    display = open_display(None)
    assert display, "The optional navigation test requires an X11 display"
    previous_error_handler=set_error(c.cast(handle_error,pointer))
    root = root_window(display)
    pid_atom = atom(display, b"_NET_WM_PID", 0)

    def find(parent, pid):
        actual, count, left, data, fmt = window(), window(), window(), pointer(), c.c_int()
        property_get(display, parent, pid_atom, 0, 1, 0, 6, c.byref(actual), c.byref(fmt),
                     c.byref(count), c.byref(left), c.byref(data))
        match = data and count.value and c.cast(data, c.POINTER(window))[0] == pid
        if data:
            free(data)
        if match:
            name = pointer()
            fetch_name(display, parent, c.byref(name))
            title = c.string_at(name).decode(errors="replace") if name else ""
            if name:
                free(name)
            # SDL may briefly recreate its X11 window while selecting a renderer.
            # Ignore unnamed setup windows and only target the final app window.
            if title.startswith("MicroPixel PC"):
                return parent
        tree_root, tree_parent, children, length = window(), window(), c.POINTER(window)(), c.c_uint()
        query(display, parent, c.byref(tree_root), c.byref(tree_parent), c.byref(children), c.byref(length))
        values = list(children[:length.value])
        if children:
            free(children)
        for child in values:
            found = find(child, pid)
            if found:
                return found
        return 0

    log_path = output / "navigation.log"
    process = None
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen([str(BINARY), "--seconds", str(seconds), "--screenshot",
                                        str(output / "navigation.png"), *map(str, arguments)], cwd=ROOT,
                                       env=dict(env, SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER=env.get("SDL_AUDIODRIVER", "dummy")),
                                       stdout=log, stderr=log)
            deadline, target = time.monotonic() + 5, 0
            while not target and time.monotonic() < deadline:
                time.sleep(0.1)
                target = find(root, process.pid)
            assert target, "PC test window did not appear"

            def event(kind, button, px=0, py=0):
                nonlocal target
                assert process.poll() is None, log_path.read_text(errors="replace")
                current = find(root, process.pid)
                assert current, "The PC window disappeared before input"
                target = current
                value = AnyEvent()
                value.event = Event(kind, 0, 1, display, target, root, 0, 0, px, py, px, py, 0, button, 1)
                assert send(display, target, 0, 64 if kind == 6 else 1 << (kind - 2), c.byref(value))
                flush(display)
                time.sleep(0.08)

            def press_key(symbol):
                event(2, keycode(display, symbol))
                event(3, keycode(display, symbol))
            event.key = press_key

            def click(px, py):
                event(6, 0, px, py)
                event(4, 1, px, py)
                event(5, 1, px, py)
                time.sleep(0.15)

            def wait_for(marker):
                deadline = time.monotonic() + wait_seconds
                while marker not in log_path.read_text(errors="replace") and time.monotonic() < deadline:
                    time.sleep(0.05)
                assert marker in log_path.read_text(errors="replace"), log_path.read_text(errors="replace")

            wait_for("SDL audio: driver=")
            time.sleep(0.3)
            if driver:
                def escape():
                    event(2, keycode(display, 0xff1b))
                    event(3, keycode(display, 0xff1b))
                driver(click, event, wait_for, log_path, process, escape)
                assert process.wait(timeout=max(20,seconds+5)) == 0, log_path.read_text(errors="replace")
                assert not x_errors,x_errors
                return
            click(640, 55)  # Local hall -> market.
            wait_for("Market catalog: 7 entries")
            click(130, 300)  # First card -> detail, interrupting pending cover work.
            wait_for("Market detail: test.market.demo0; 2 releases")
            click(80, 45)  # Detail -> cached catalog.
            click(650, 620)  # Next page.
            click(650, 45)  # Refresh catalog while cover work is active.
            deadline = time.monotonic() + 3
            while Handler.requests.count("/api/v1/store/apps") < 2 and time.monotonic() < deadline:
                time.sleep(0.05)
            assert Handler.requests.count("/api/v1/store/apps") == 2, Handler.requests
            click(80, 45)  # Market -> local hall.
            click(640, 55)  # Reopen cached market.
            event(2, keycode(display, 0xff1b))  # Esc -> local hall.
            event(3, keycode(display, 0xff1b))
            assert process.wait(timeout=15) == 0, log_path.read_text(errors="replace")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        close(display)
        set_error(previous_error_handler)


class Handler(http.server.BaseHTTPRequestHandler):
    mode = "good"
    requests = []

    def log_message(self, *_):
        pass

    def do_GET(self):
        Handler.requests.append(self.path)
        time.sleep(0.15)  # The UI must keep rendering while the response is pending.
        status = 200
        if self.path == "/cover.png":
            data = COVER
        elif self.path.startswith("/api/v1/store/apps/test.market.demo"):
            app = dict(APP, appId=self.path.rsplit("/", 1)[1])
            data = json.dumps(app, ensure_ascii=False).encode()
        elif Handler.mode == "invalid":
            data = b"<html>not JSON</html>"
        elif Handler.mode == "error":
            status, data = 503, b"Unavailable"
        else:
            apps = []
            if Handler.mode != "empty":
                for i in range(129 if Handler.mode == "oversized" else 7):
                    app = copy.deepcopy(APP)
                    app["appId"] += str(i)
                    apps.append(app)
            data = json.dumps({"apps": apps}).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="pc-market-", dir=ROOT / "build") as directory:
            output = Path(directory)
            env = dict(os.environ, MICROPIXEL_PC_MARKET_URL=f"http://127.0.0.1:{server.server_port}",
                       MICROPIXEL_PC_DATA_DIR=str(output / "storage"))
            if "--interactive" in sys.argv:
                navigate_x11(env, output)
                print("PASS PC X11 hall/market/detail navigation, pagination, refresh, cancellation and Esc")
                return

            def run(name, *args, marker):
                Handler.mode = name
                Handler.requests.clear()
                screenshot = output / f"{name}.png"
                result = subprocess.run([str(BINARY), "--headless", "--seconds", "2", "--screenshot",
                                         str(screenshot), *args], cwd=ROOT, env=env, capture_output=True,
                                        timeout=15, check=True)
                log = result.stderr.decode(errors="replace")
                assert marker in log, log
                with Image.open(screenshot) as image:
                    assert image.size == (720, 720)
                    # The real decoded and resampled HTTP cover must reach LVGL.
                    if name in ("good", "detail"):
                        pixels = list(image.convert("RGB").getdata())
                        assert sum(r > 200 and g < 60 and b > 120 for r, g, b in pixels) > 2000, log
                return log

            run("good", "--market", marker="Market catalog: 7 entries")
            assert Handler.requests.count("/cover.png") == 4, Handler.requests
            run("detail", "--market-app", APP["appId"], marker="Market detail: test.market.demo; 2 releases")
            assert f'/api/v1/store/apps/{APP["appId"]}' in Handler.requests
            run("empty", "--market", marker="Market catalog: 0 entries")
            run("invalid", "--market", marker="Market error: Invalid market JSON")
            run("oversized", "--market", marker="Market error: Invalid or oversized market catalog")
            run("error", "--market", marker="Market error: HTTP status 503")
            run("hall", marker="SDL audio: driver=")
            assert not Handler.requests, "Local hall unexpectedly contacted the market"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS PC market catalog/detail rendering, covers, withdrawn releases, empty/error/oversize states and local hall")


if __name__ == "__main__":
    main()
