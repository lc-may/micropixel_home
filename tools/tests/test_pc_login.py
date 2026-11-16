#!/usr/bin/env python3
"""Guest website/WeChat login flow using loopback fixtures; no real account."""
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
import urllib.parse
from PIL import Image, ImageDraw
from test_pc_market import navigate_x11
ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("MICROPIXEL_PC_BINARY", ROOT / "build/pc/micropixel-pc"))
TOKEN="fixture-auth-secret-never-log"
CSRF="fixture-csrf-never-log"
class Handler(http.server.BaseHTTPRequestHandler):
    mode="success"
    polls=0
    poll_started=threading.Event()
    release_poll=threading.Event()
    hits=[]
    failures=[]
    image=b""
    png=b""
    oversized=b""
    def log_message(self,*_): pass
    def reply(self,data=b"",status=200,headers=()):
        self.send_response(status)
        self.send_header("Content-Length",str(len(data)))
        for k,v in headers:self.send_header(k,v)
        self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
    def do_GET(self):
        path=urllib.parse.urlsplit(self.path).path
        Handler.hits.append(path)
        try:
            self.check_host()
            cookie=self.headers.get("Cookie","")
            if path.startswith(("/__wx","/__poll")):
                assert "MUSIC_U" not in cookie and "MUSIC_SESSION" not in cookie,"NetEase cookie leaked"
            if path=="/api/sns/authorize":
                Handler.polls=0
                location="https://open.weixin.qq.com/connect/qrconnect?appid=fixture-app&redirect_uri=https%3A%2F%2Fmusic.163.com%2Fback%2Fweichat&state=fixture-state"
                if Handler.mode=="evil": location="https://example.com/credential-theft"
                self.reply(status=302,headers=[("Location",location),("Set-Cookie","MUSIC_SESSION=music-only; Path=/; Secure; HttpOnly")]);return
            if path=="/__wx/connect/qrconnect":
                flag="0" if Handler.mode=="legacy" else "1"
                page=f'<html><script>var usenewdomain = \'{flag}\' * 1 || 0;</script><img src="/connect/qrcode/fixture"></html>'
                self.reply(page.encode(),headers=[("Set-Cookie","WX_SESSION=wechat-only; Path=/connect; Secure")]);return
            if path=="/__wx/connect/qrcode/fixture":self.reply(Handler.image);return
            if path in ("/__poll/connect/l/qrconnect","/__poll-new/connect/l/qrconnect"):
                assert (path.startswith("/__poll-new/")== (Handler.mode!="legacy")),"polling domain differs from official page flag"
                assert self.headers.get("Referer")=="https://open.weixin.qq.com/","WeChat polling referer"
                assert "_" in urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query),"poll cache nonce missing"
                Handler.poll_started.set()
                Handler.polls+=1
                code=408
                if Handler.mode=="expired":code=400
                elif Handler.mode=="cancelled":code=403
                elif Handler.mode not in ("wait","slow"):
                    code=404 if Handler.polls==1 else 405
                if Handler.mode=="slow":Handler.release_poll.wait(20)
                if Handler.mode=="longpoll" and Handler.polls==1:time.sleep(12)
                script=f'window.wx_errcode={code};window.wx_code="fixture-code";'
                self.reply(script.encode());return
            if path=="/back/weichat":
                query=urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                assert query.get("code")==["fixture-code"] and query.get("state")==["fixture-state"],"OAuth state/code lost"
                assert "MUSIC_SESSION=music-only" in cookie and "WX_SESSION" not in cookie,"callback cookie isolation"
                self.reply(b"ok",headers=[("Set-Cookie",f"MUSIC_U={TOKEN}; Domain=.163.com; Path=/; Secure; HttpOnly"),("Set-Cookie",f"__csrf={CSRF}; Path=/; Secure")]);return
            if path=="/__contract/exchange":
                assert self.headers.get("X-Contract")=="yes"
                self.reply(status=302,headers=[("Location","https://example.com/never-follow"),("Set-Cookie","a=1; Path=/"),("Set-Cookie","b=2; Path=/")]);return
            if path=="/__contract/delay":time.sleep(.35);self.reply(b"delayed");return
            if path=="/__contract/png":self.reply(Handler.png);return
            if path=="/__contract/oversize":self.reply(Handler.oversized);return
            self.reply(b"unexpected",404)
        except AssertionError as error:Handler.failures.append(str(error));self.reply(b"fixture rejected",400)
    def do_POST(self):
        path=urllib.parse.urlsplit(self.path).path;Handler.hits.append(path)
        length=int(self.headers.get("Content-Length",0));body=self.rfile.read(length)
        try:
            self.check_host()
            assert body.startswith(b"params=") and b"encSecKey=" in body
            cookie=self.headers.get("Cookie","")
            if path=="/weapi/w/nuser/account/get":
                assert f"MUSIC_U={TOKEN}" in cookie
                result={"code":200,"account":{"id":42},"profile":{"userId":42,"nickname":"微信测试用户"}}
                if Handler.mode=="invalid":result={"code":200,"account":None,"profile":None}
            elif path=="/weapi/search/get":
                if Handler.mode=="success":assert f"MUSIC_U={TOKEN}" in cookie
                assert "WX_SESSION" not in cookie
                result={"code":301} if Handler.mode=="expired-api" else {"code":200,"result":{"songCount":0}}
            elif path=="/weapi/song/like/get":
                result={"code":200,"ids":[]}
            elif path=="/weapi/user/playlist":
                assert f"MUSIC_U={TOKEN}" in cookie
                result={"code":301} if Handler.mode=="expired-api" else {"code":200,"playlist":[],"more":False}
            else:raise AssertionError("unexpected POST")
            self.reply(json.dumps(result,ensure_ascii=False).encode())
        except AssertionError as error:Handler.failures.append(str(error));self.reply(b"fixture rejected",400)
    def check_host(self):
        port=self.server.server_port
        expected="127.0.0.1" if port==80 else f"127.0.0.1:{port}"
        assert self.headers.get("Host")==expected,"default HTTP port leaked into Host, or custom port lost"
def images():
    # The generated test image is only served when --test-origin explicitly points to loopback.
    image=Image.new("RGB",(470,470),"white");draw=ImageDraw.Draw(image)
    for y in range(30,440,10):
        for x in range(30,440,10):
            if ((x//10)*7+(y//10)*13)%5<2:draw.rectangle((x,y,x+9,y+9),fill="black")
    out=io.BytesIO();image.save(out,"JPEG");Handler.image=out.getvalue()
    out=io.BytesIO();Image.new("RGB",(32,32),(20,160,70)).save(out,"PNG");Handler.png=out.getvalue()
    out=io.BytesIO();Image.new("RGB",(513,1),"white").save(out,"JPEG");Handler.oversized=out.getvalue()
def main():
    images();server=http.server.ThreadingHTTPServer(("127.0.0.1",80 if "--default-port" in sys.argv else 0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    results=ROOT/"build/pc-login-tests";results.mkdir(parents=True,exist_ok=True)
    bundle=ROOT/"build/pc-rv32-apps/netease-music/netease-music.bundle.bin"
    extra=[str(bundle)] if "--rv32" in sys.argv else []
    try:
        with tempfile.TemporaryDirectory(prefix="pc-login-",dir=ROOT/"build") as temporary:
            output=Path(temporary);origin=f"http://127.0.0.1:{server.server_port}"
            env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=str(output/"data"))
            args=["--guest-arg","--test-origin","--guest-arg",origin]
            def run(name,*arguments,seconds=5):
                if "--rv32" in sys.argv: seconds += 8  # Separate allowance for QEMU cold startup.
                result=subprocess.run([str(BINARY),"--headless","--seconds",str(seconds),*map(str,arguments),*args],cwd=ROOT,env=env,capture_output=True,timeout=max(20,seconds+8))
                log=result.stderr.decode(errors="replace");(results/f"{name}.log").write_text(log)
                assert result.returncode==0,log
                assert TOKEN not in log and CSRF not in log and "fixture-code" not in log,"credential leaked to logs"
                return log
            Handler.mode="wait"
            log=run("contract","--autostart","--exit-on-app-complete","build/pc-apps/auth_contract/auth_contract.pc.json",seconds=10)
            assert "AUTH_CONTRACT PASS" in log,log
            if "--interactive" in sys.argv:
                Handler.mode="success"
                def drive(click,event,wait_for,log_path,process,escape):
                    wait_for("NETEASE_LOGIN waiting");click(180,674)  # Remember is explicit opt-in.
                    wait_for("NETEASE_LOGIN authenticated")
                    wait_for("NETEASE_HOME playlists: 0")
                    click(592,680);wait_for("NETEASE_GUEST search: 0 tracks")
                navigate_x11(env,output,["--music-login",*args,*extra],drive,seconds=8)
                login_log=(output/"navigation.log").read_text();assert TOKEN not in login_log and CSRF not in login_log
                log=run("restore","--music",*extra,seconds=2)
                assert "NETEASE_LOGIN authenticated" in log,log
                Handler.mode="invalid"
                log=run("expired-session","--music",*extra,seconds=2)
                assert "NETEASE_LOGIN error" in log,log
                Handler.mode="wait"
                log=run("cleared-session","--music",*extra,seconds=1)
                assert "NETEASE_LOGIN authenticated" not in log and "NETEASE_LOGIN error" not in log,log
                def cancel(click,event,wait_for,log_path,process,escape):
                    wait_for("NETEASE_LOGIN waiting");assert Handler.poll_started.wait(3),"long poll never started"
                    started=time.monotonic();click(65,40)
                    wait_for("NETEASE_GUEST search: 0 tracks");assert time.monotonic()-started<1.5,"cancel blocked event loop"
                    Handler.release_poll.set()
                Handler.mode="slow";Handler.poll_started.clear();Handler.release_poll.clear();navigate_x11(env,output,["--music-login",*args,*extra],cancel,seconds=5)
                print("PASS login interaction, opt-in persistence, restored session verification, expiry clearing and cancellation")
            else:
                cases=[("longpoll","NETEASE_LOGIN authenticated")] if "--long-poll" in sys.argv else [("legacy","NETEASE_LOGIN authenticated"),("success","NETEASE_LOGIN authenticated"),("expired","NETEASE_LOGIN expired"),("cancelled","NETEASE_LOGIN cancelled"),("evil","NETEASE_LOGIN error"),("wait","NETEASE_LOGIN waiting"),("expired-api","登录已失效，请重新扫码")]
                for mode,marker in cases:
                    Handler.mode=mode
                    log=run(mode,"--music-login",*extra,"--screenshot",results/f"{mode}.png",seconds=17 if mode=="longpoll" else 5)
                    assert marker in log,log
                assert "/credential-theft" not in Handler.hits
                with Image.open(results/("longpoll.png" if "--long-poll" in sys.argv else "wait.png")) as image:
                    pixels=list(image.convert("RGB").getdata())
                    if "--long-poll" not in sys.argv:assert sum(r>240 and g>240 and b>240 for r,g,b in pixels)>60000,"QR image did not reach Guest scene"
                print("PASS website/WeChat QR, scanned/authorized/expired/cancelled states, redirect isolation and generic SDK contracts")
            assert not Handler.failures,Handler.failures
    finally:Handler.release_poll.set();server.shutdown();server.server_close();thread.join()
if __name__=="__main__":main()
