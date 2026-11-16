#!/usr/bin/env python3
"""Logged-in Guest home, personal API isolation, pagination and playback."""
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
from PIL import Image
from PIL import ImageChops
from test_pc_login import Handler as LoginHandler, TOKEN, CSRF, images, BINARY, ROOT
from test_pc_market import navigate_x11
from test_pc_music import mp3


class HomeHandler(LoginHandler):
    home_mode = "normal"
    playlist_page = 0
    song_page = 0
    resolve_id = 101
    liked={101}
    like_target=1001
    like_calls=0
    like_started=threading.Event()
    release_like=threading.Event()
    recommendation_batches=None
    recommendation_requests=0
    recommendation_wait=0
    recommendation_fail=0
    search_batches=None
    search_requests=0
    recommendation_started=threading.Event()
    release_recommendation=threading.Event()
    expect_anonymous=False
    audio = b""
    avatar = b""
    personal_hits = []
    delayed = threading.Event()
    release = threading.Event()

    def data(self, value):
        self.reply(json.dumps(value, ensure_ascii=False).encode())

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/__avatar":
            assert not self.headers.get("Cookie"), "account cookies leaked to image CDN"
            HomeHandler.personal_hits.append(path)
            self.reply(b"bad image" if self.home_mode in ("bad-avatar","empty-bad-avatar") else self.avatar)
        elif path=="/__cover":
            assert not self.headers.get("Cookie"),"account cookie leaked to cover CDN"
            self.reply(self.avatar)
        elif path == "/tone.mp3":
            self.reply(self.audio)
        else:
            super().do_GET()

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        routes = ("/weapi/w/nuser/account/get", "/weapi/user/playlist",
                  "/weapi/v3/discovery/recommend/songs", "/weapi/v6/playlist/detail",
                  "/weapi/v3/song/detail", "/weapi/song/enhance/player/url", "/weapi/search/get",
                  "/weapi/song/like/get","/weapi/radio/like")
        if path not in routes:
            return super().do_POST()
        HomeHandler.personal_hits.append(path)
        try:
            self.check_host()
            if path!="/weapi/search/get":
                assert f"MUSIC_U={TOKEN}" in self.headers.get("Cookie", ""), "personal request missing session"
            elif self.expect_anonymous:
                assert "MUSIC_U" not in self.headers.get("Cookie",""),"old account cookie survived logout"
            assert "WX_SESSION" not in self.headers.get("Cookie", ""), "WeChat cookie leaked"
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            assert body.startswith(b"params=") and b"encSecKey=" in body
            mode = self.home_mode
            if path=="/weapi/song/like/get":
                self.data({"code":200,"ids":sorted(self.liked)})
            elif path=="/weapi/radio/like":
                HomeHandler.like_calls+=1
                if mode=="like-slow":
                    self.like_started.set();self.release_like.wait(10)
                if mode=="like-fail":return self.data({"code":500,"message":"fixture denied"})
                if self.like_target in self.liked:self.liked.remove(self.like_target)
                else:self.liked.add(self.like_target)
                self.data({"code":200})
            elif path=="/weapi/search/get":
                if not self.search_batches:self.data({"code":200,"result":{"songCount":0}})
                else:
                    page=min(self.search_requests,len(self.search_batches)-1)
                    HomeHandler.search_requests+=1
                    songs=[self.track(i) for i in self.search_batches[page]]
                    for song in songs:song.pop("metadata",None)
                    self.data({"code":200,"result":{"songCount":sum(map(len,self.search_batches)),"songs":songs}})
            elif path == "/weapi/w/nuser/account/get":
                self.data({"code": 200, "account": {"id": 42},
                           "profile": {"userId": 43 if mode == "mismatch" else 42, "nickname": "小云的音乐",
                                       "avatarUrl": "https://p1.music.126.net/home.jpg",
                                       "signature": "让音乐陪伴每一天\n听见好心情"}})
            elif path == "/weapi/user/playlist":
                if mode == "malformed":
                    return self.data({"code": 200, "playlist": {}, "more": True})
                if mode in ("empty","empty-bad-avatar"):
                    return self.data({"code": 200, "playlist": [], "more": False})
                if mode == "slow":
                    self.delayed.set()
                    self.release.wait(10)
                count = 20 if self.playlist_page == 0 else 3
                self.data({"code": 200, "playlist": [
                    {"id": 501 + self.playlist_page*20 + i, "name": "我喜欢的音乐" if i == 0 else f"午后收藏 {i}",
                     "trackCount": 25, "creator": {"userId": 42 if i % 2 == 0 else 77}}
                    for i in range(count)], "more": self.playlist_page == 0})
            elif path == "/weapi/v3/discovery/recommend/songs":
                if mode == "expired":
                    return self.data({"code": 301})
                HomeHandler.recommendation_requests+=1
                request=HomeHandler.recommendation_requests
                if request==self.recommendation_wait:
                    self.recommendation_started.set();self.release_recommendation.wait(10)
                if request==self.recommendation_fail:return self.data({"code":500,"message":"fixture retry"})
                ids=self.recommendation_batches[min(request-1,len(self.recommendation_batches)-1)] if self.recommendation_batches else range(1001,1031)
                self.data({"code": 200, "data": {"dailySongs": [self.track(i) for i in ids]}})
            elif path == "/weapi/v6/playlist/detail":
                self.data({"code": 200, "playlist": {"id": 501, "trackCount": 25,
                           "trackIds": [{"id": i} for i in range(101, 126)]}})
            elif path == "/weapi/v3/song/detail":
                start = 101 + self.song_page * 20
                self.data({"code": 200, "songs": [self.track(i) for i in range(start, min(start + 20, 126))]})
            else:
                self.data({"code": 200, "data": [{"id": self.resolve_id,
                           "url": f"http://127.0.0.1:{self.server.server_port}/tone.mp3", "freeTrialInfo": None}]})
        except AssertionError as error:
            LoginHandler.failures.append(str(error))
            self.reply(b"fixture rejected", 400)

    @staticmethod
    def track(i):
        # Real song responses contain many fields unrelated to presentation.
        return {"id": i, "name": f"今日心动 {i}", "ar": [{"name": "云端乐队"}], "dt": 8000,
                "al":{"picUrl":"https://p1.music.126.net/cover.jpg"},
                "metadata": {f"field{n}": n for n in range(100)}}


def main():
    images()
    image = io.BytesIO()
    Image.new("RGB", (96, 96), (205, 65, 83)).save(image, "PNG")
    HomeHandler.avatar, HomeHandler.audio = image.getvalue(), mp3(8)
    LoginHandler.mode = "success"
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HomeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = ROOT / "build/pc-home-tests"
    results.mkdir(parents=True, exist_ok=True)
    extra = [str(ROOT / "build/pc-rv32-apps/netease-music/netease-music.bundle.bin")] if "--rv32" in sys.argv else []
    try:
        with tempfile.TemporaryDirectory(prefix="pc-home-", dir=ROOT / "build") as directory:
            output = Path(directory)
            env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=str(output / "data"))
            args = ["--music-login", "--guest-arg", "--test-origin", "--guest-arg",
                    f"http://127.0.0.1:{server.server_port}", *extra]

            def run(mode):
                HomeHandler.home_mode = mode
                seconds=10 if extra else 5
                result = subprocess.run([str(BINARY), "--headless", "--seconds", str(seconds), *args,
                                         "--screenshot", str(results / f"{mode}.png")],
                                        cwd=ROOT, env=env, capture_output=True, timeout=seconds+10)
                log = result.stderr.decode(errors="replace")
                (results / f"{mode}.log").write_text(log)
                assert result.returncode == 0, log
                assert all(secret not in log for secret in (TOKEN, CSRF, "fixture-code")), "credential logged"
                return log

            if "--interactive" not in sys.argv:
                log = run("normal")
                assert "NETEASE_HOME playlists: 20" in log and "NETEASE_HOME avatar ready" in log, log
                with Image.open(results / "normal.png") as screenshot:
                    assert screenshot.getpixel((50, 104))[:3] == (205, 65, 83), "profile avatar not rendered"
                assert "NETEASE_HOME playlists: 0" in run("empty")
                assert "NETEASE_HOME playlists: 0" in run("empty-bad-avatar")
                with Image.open(results/"empty.png") as good,Image.open(results/"empty-bad-avatar.png") as bad:
                    assert ImageChops.difference(good.crop((80,320,640,380)),bad.crop((80,320,640,380))).getbbox() is None,"avatar failure hid empty-library state"
                assert "歌单列表格式无效" in run("malformed")
                log = run("bad-avatar")
                assert "NETEASE_HOME playlists: 20" in log and "NETEASE_HOME avatar ready" not in log
                log = run("mismatch")
                assert "NETEASE_LOGIN error" in log and "NETEASE_HOME playlists:" not in log
            if "--interactive" in sys.argv:
                HomeHandler.home_mode="normal"
                pcm=output/"home.pcm"
                interactive_env=dict(env,SDL_AUDIODRIVER="disk",SDL_DISKAUDIOFILE=str(pcm))
                def drive(click,event,wait_for,log_path,process,escape):
                    def fresh(marker,action):
                        offset=len(log_path.read_text(errors="replace"));action()
                        deadline=time.monotonic()+(10 if extra else 4)
                        while marker not in log_path.read_text(errors="replace")[offset:] and time.monotonic()<deadline:time.sleep(.03)
                        assert marker in log_path.read_text(errors="replace")[offset:],log_path.read_text()
                    wait_for("NETEASE_HOME avatar ready")
                    # My playlists -> songs -> full player.
                    fresh("NETEASE_HOME tracks: 20",lambda:click(240,305))
                    fresh("Audio playing:",lambda:click(240,229))
                    deadline=time.monotonic()+3
                    while (not pcm.exists() or not any(pcm.read_bytes())) and time.monotonic()<deadline:time.sleep(.03)
                    assert pcm.exists() and any(pcm.read_bytes()),"playlist playback silent"
                    fresh("已暂停",lambda:click(360,326))
                    commits=log_path.read_text().count("NETEASE_GUEST seek:")
                    event(6,0,64,582);event(4,1,64,582);event(6,0,260,582);event(6,0,360,582)
                    assert log_path.read_text().count("NETEASE_GUEST seek:")==commits,"drag sought before release"
                    fresh("NETEASE_GUEST seek:",lambda:event(5,1,360,582))
                    assert log_path.read_text().count("NETEASE_GUEST seek:")==commits+1,"seek committed more than once"
                    click(50,40)
                    fresh("NETEASE_HOME recommendations: 30",lambda:click(128,680))
                    # The playlist queue uses 101/102, recommendations use 1001/1002.
                    HomeHandler.resolve_id=102
                    fresh("NETEASE_GUEST selected: 102",lambda:click(650,612))
                    fresh("已暂停",lambda:click(584,612))
                    # A scroll gesture must not favorite the row where it ends.
                    count=HomeHandler.like_calls
                    event(6,0,652,460);event(4,1,652,460);event(6,0,652,220);event(5,1,652,220)
                    assert HomeHandler.like_calls==count,"scroll changed favorites"
                    fresh("NETEASE_HOME recommendations: 30",lambda:click(572,38))
                    HomeHandler.home_mode="like-slow";HomeHandler.like_target=1001
                    HomeHandler.like_started.clear();HomeHandler.release_like.clear()
                    click(652,229);assert HomeHandler.like_started.wait(3),"favorite request missing"
                    click(652,229)
                    # Navigation cannot cancel an in-flight write.
                    click(360,680)
                    assert HomeHandler.like_calls==count+1,"pending favorite was duplicated"
                    HomeHandler.home_mode="normal";HomeHandler.release_like.set()
                    wait_for("NETEASE_LIKE confirmed: liked=1");wait_for("NETEASE_HOME playlists: 20")
                    fresh("NETEASE_HOME recommendations: 30",lambda:click(128,680))
                    assert 1001 in HomeHandler.liked
                navigate_x11(interactive_env,output,args,drive,seconds=45 if extra else 30,wait_seconds=12 if extra else 8)
                (results/"interactive.log").write_text((output/"navigation.log").read_text())
                (results/"mobile-liked.png").write_bytes((output/"navigation.png").read_bytes())
                with Image.open(results/"mobile-liked.png") as screenshot:
                    red=sum(r>180 and g<110 and b<150 for r,g,b in screenshot.convert("RGB").crop((628,198,686,260)).getdata())
                    assert red>40,"confirmed favorite star is not red"
                def unlike_and_logout(click,event,wait_for,log_path,process,escape):
                    def fresh(marker,action):
                        offset=len(log_path.read_text(errors="replace"));action()
                        deadline=time.monotonic()+(10 if extra else 4)
                        while marker not in log_path.read_text(errors="replace")[offset:] and time.monotonic()<deadline:time.sleep(.03)
                        assert marker in log_path.read_text(errors="replace")[offset:],log_path.read_text()
                    wait_for("NETEASE_HOME avatar ready")
                    fresh("NETEASE_HOME recommendations: 30",lambda:click(128,680))
                    HomeHandler.home_mode="like-fail";HomeHandler.like_target=1001
                    fresh("NETEASE_LIKES loaded: 2",lambda:click(652,229))
                    assert "收藏状态未确认" in log_path.read_text()
                    assert 1001 in HomeHandler.liked,"failed unlike changed server state"
                    HomeHandler.home_mode="normal"
                    fresh("NETEASE_LIKE confirmed: liked=0",lambda:click(652,229))
                    assert 1001 not in HomeHandler.liked
                    fresh("NETEASE_HOME playlists: 20",lambda:click(360,680))
                    fresh("NETEASE_HOME tracks: 1",lambda:click(300,186))
                    click(650,38);HomeHandler.expect_anonymous=True
                    fresh("已退出登录",lambda:click(600,674))
                    click(65,40)
                    fresh("NETEASE_GUEST search: 0 tracks",lambda:click(592,680))
                navigate_x11(env,output,args,unlike_and_logout,seconds=35 if extra else 20,wait_seconds=12 if extra else 8)
                HomeHandler.expect_anonymous=False
                def expired(click,event,wait_for,log_path,process,escape):
                    wait_for("NETEASE_HOME avatar ready");HomeHandler.home_mode="expired"
                    click(128,680);wait_for("登录已失效，请重新扫码")
                    calls=len(HomeHandler.personal_hits);click(240,229);click(650,612);time.sleep(.25)
                    assert len(HomeHandler.personal_hits)==calls,"expired private data remained interactive"
                HomeHandler.home_mode="normal"
                navigate_x11(env,output,args,expired,seconds=15 if extra else 12,wait_seconds=12 if extra else 8)
                HomeHandler.home_mode="slow";HomeHandler.delayed.clear();HomeHandler.release.clear()
                def cancel(click,event,wait_for,log_path,process,escape):
                    assert HomeHandler.delayed.wait(10 if extra else 4),"delayed personal request missing"
                    started=time.monotonic();click(592,680);wait_for("NETEASE_GUEST search: 0 tracks")
                    assert time.monotonic()-started<1.5,"home cancellation blocked UI"
                    HomeHandler.release.set()
                navigate_x11(env,output,args,cancel,seconds=15 if extra else 12,wait_seconds=12 if extra else 8)
            assert not LoginHandler.failures, LoginHandler.failures
    finally:
        HomeHandler.release.set()
        HomeHandler.release_like.set()
        server.shutdown()
        server.server_close()
        thread.join()
    if "--interactive" not in sys.argv:
        print("PASS authenticated profile/avatar, personal playlists, empty/error states, account identity and cookie isolation")
    if "--interactive" in sys.argv:
        print("PASS mobile lists/scrolling, player seek/pause, queue retention, favorite confirmation/failure/duplicates, logout and expiry")


if __name__ == "__main__":
    main()
