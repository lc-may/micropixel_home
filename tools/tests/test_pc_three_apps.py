#!/usr/bin/env python3
"""Three actual application Guests, Host alarm/input/focus and optional 10-minute soak."""
from test_pc_market import close_home_app, open_home_app, open_home_market
import array,http.server,json,math,os,re,subprocess,sys,tempfile,threading,time,urllib.parse
from pc_test_backend import app_path
from pathlib import Path
from test_pc_components import descriptor
from test_pc_home import HomeHandler
from test_pc_login import Handler as LoginHandler,TOKEN,CSRF,images
from test_pc_market import ROOT,BINARY,navigate_x11
from test_pc_music import mp3
class Handler(HomeHandler):
    weather_requests=0
    resolves=0
    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path=="/weapi/song/enhance/player/url":
            HomeHandler.resolve_id=1001+Handler.resolves;Handler.resolves+=1
        return super().do_POST()
    def do_GET(self):
        if urllib.parse.urlsplit(self.path).path=="/weather":
            Handler.weather_requests+=1
            return self.data({"current":{"time":int(time.time()),"temperature_2m":21.5+Handler.weather_requests/10,"weather_code":1}})
        return super().do_GET()

def main():
    soak="--soak" in sys.argv;duration=650 if soak else 45
    images();LoginHandler.mode="success";HomeHandler.audio=mp3(40);HomeHandler.avatar=LoginHandler.png;HomeHandler.resolve_id=1001
    server=http.server.ThreadingHTTPServer(("127.0.0.1",0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    results=ROOT/("build/pc-three-app-soak" if soak else "build/pc-alarm-tests");results.mkdir(exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="data-",dir=results) as temporary:
            directory=Path(temporary);paths=[]
            for prefix,name in (("a","weather"),("b","clock-alarm"),("c","netease-music")):
                paths.append(descriptor(app_path(name),directory/f"{prefix}-{name}.pc.json",f"micropixel.{name}"))
            pcm=results/"combined.pcm";env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=str(directory/"storage"),SDL_AUDIODRIVER="disk",SDL_DISKAUDIOFILE=str(pcm))
            def driver(click,event,wait_for,log_path,process,escape):
                def wait_new(marker,action,timeout=8):
                    offset=len(log_path.read_text());action();until=time.monotonic()+timeout
                    while marker not in log_path.read_text()[offset:] and time.monotonic()<until:time.sleep(.05)
                    assert marker in log_path.read_text()[offset:],log_path.read_text()[-14000:]
                def sound():
                    time.sleep(.05);offset=pcm.stat().st_size;time.sleep(.35)
                    with pcm.open("rb") as f:f.seek(offset);data=f.read(48000*4//4)
                    return any(data)
                def tones():
                    offset=pcm.stat().st_size;time.sleep(.15)
                    with pcm.open("rb") as f:f.seek(offset);data=f.read(4800*4)
                    samples=array.array("h",data);left=samples[::2];n=len(left);assert n==4800
                    def amplitude(hz):return abs(sum(value*complex(math.cos(2*math.pi*hz*i/48000),math.sin(2*math.pi*hz*i/48000)) for i,value in enumerate(left)))*2/n
                    return amplitude(440),amplitude(880)
                wait_for("WEATHER refresh success");wait_for("CLOCK service ready")
                wait_new("CLOCK activity created",lambda:open_home_app(click, 1))
                wait_new("CLOCK test alarm registered",lambda:click(510,475))
                wait_new("APP_SHELL stopped: micropixel.clock-alarm",lambda:close_home_app(click,escape))
                assert "ALARM overlay visible" not in log_path.read_text()
                open_home_app(click, 2);wait_for("NETEASE_LOGIN authenticated");wait_for("NETEASE_HOME playlists:")
                click(120,680);wait_for("NETEASE_HOME recommendations: 30");click(140,220);wait_for("Audio playing:");baseline=tones()[0]
                wait_for("ALARM overlay visible");wait_for("ALARM focus active=1");wait_for("NETEASE_FOCUS ducked")
                during=tones();assert baseline>100 and during[0]<baseline*.4 and during[1]>100,(baseline,during)
                assert "APP_SHELL started: micropixel.clock-alarm" in log_path.read_text()
                assert log_path.read_text().count("APP_SHELL started: micropixel.clock-alarm")==1,"Host alarm restarted a user-stopped Clock Guest"
                seek_count=log_path.read_text().count("NETEASE_GUEST seek:");click(650,582);assert log_path.read_text().count("NETEASE_GUEST seek:")==seek_count
                wait_new("NETEASE_WIDGET playing=0",lambda:click(350,480))
                wait_new("ALARM overlay closed",lambda:click(350,390))
                wait_for("ALARM focus active=0");assert not sound(),"focus release revived user-paused music"
                event.key(0xffbe)
                wait_new("HOME_MEDIA paused=0",lambda:click(548,606));assert sound()
                home_start=time.monotonic();ticks=0
                print("D7 combined playback soak started" if soak else "D7 alarm integration started",flush=True)
                while soak and time.monotonic()-home_start<600:
                    time.sleep(30);ticks+=1
                    assert process.poll() is None,log_path.read_text()[-14000:]
                    assert sound(),"unexpected background PCM silence"
                    if ticks%2==0:
                        text=log_path.read_text();match=re.search(r"HOME_WIDGET snapshot: app=0 template=1 .*index=(\d+)",text);assert match,text[-10000:]
                        index=int(match.group(1));x=270 if index==0 else 610
                        wait_new("WEATHER refresh success",lambda:click(660,96))
                    assert "Guest failed:" not in log_path.read_text(),log_path.read_text()[-14000:]
                    print(f"D7 combined soak {ticks*30}s: PCM active, weather refreshes={Handler.weather_requests}",flush=True)
                if soak:assert log_path.read_text().count("Audio playing:")>=10,"automatic next-track stopped"
                assert "APP_SHELL stopped: micropixel.netease-music" not in log_path.read_text()
                assert log_path.read_text().count("NETEASE_LOGIN authenticated")==1
            args=["--home","--guest-arg","--login","--guest-arg","--test-origin","--guest-arg",f"http://127.0.0.1:{server.server_port}","--guest-arg","--weather-origin","--guest-arg",f"http://127.0.0.1:{server.server_port}/weather",*paths]
            navigate_x11(env,results,args,driver=driver,seconds=duration,wait_seconds=15)
            text=(results/"navigation.log").read_text();assert "Guest failed:" not in text and TOKEN not in text and CSRF not in text,text[-14000:]
            # The Host persistence remains after the Clock instance was closed.
            import sqlite3
            db=sqlite3.connect(directory/"storage/schedules.sqlite3");row=db.execute("SELECT plan FROM alarms WHERE app='micropixel.clock-alarm' AND id=2").fetchone();assert row and len(row[0])==88;db.close()
        print("PASS three real Guests, stopped Clock Host alarm, modal input, focus duck/user pause and "+("600s combined playback" if soak else "music resume"))
    finally:server.shutdown();server.server_close()
if __name__=="__main__":main()
