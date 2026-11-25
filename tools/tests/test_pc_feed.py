#!/usr/bin/env python3
"""Infinite-scroll integration, distinct recommendation batches and failure recovery."""
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
from test_pc_market import navigate_x11
from test_pc_music import mp3

def main():
    images();LoginHandler.mode="success"
    picture=io.BytesIO();Image.new("RGB",(96,96),(205,65,83)).save(picture,"PNG")
    HomeHandler.avatar=picture.getvalue();HomeHandler.audio=mp3(8)
    server=http.server.ThreadingHTTPServer(("127.0.0.1",0),HomeHandler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    extra=[str(ROOT/"build/pc-rv32-apps/netease-music/netease-music.bundle.bin")] if "--rv32" in sys.argv else []
    results=ROOT/"build/pc-feed-tests";results.mkdir(exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="pc-feed-",dir=ROOT/"build") as directory:
            output=Path(directory);env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=str(output/"data"))
            args=["--music-login","--guest-arg","--test-origin","--guest-arg",f"http://127.0.0.1:{server.server_port}",*extra]
            def scenario(name,driver,seconds):
                navigate_x11(env,output,args,driver,seconds=seconds,wait_seconds=10 if extra else 4)
                log=(output/"navigation.log").read_text()
                assert TOKEN not in log and CSRF not in log
                (results/f"{name}.log").write_text(log)
                (results/f"{name}.png").write_bytes((output/"navigation.png").read_bytes())
            def helpers(click,event,log_path):
                def swipe():
                    event(6,0,400,570);event(4,1,400,570);event(6,0,400,255);event(5,1,400,255)
                def wait(marker,action=lambda:None):
                    offset=len(log_path.read_text());action();deadline=time.monotonic()+(10 if extra else 5)
                    while marker not in log_path.read_text()[offset:] and time.monotonic()<deadline:time.sleep(.03)
                    assert marker in log_path.read_text()[offset:],log_path.read_text()
                def bottom(predicate):
                    for _ in range(18):
                        if predicate():return
                        swipe();time.sleep(.04)
                    assert predicate(),log_path.read_text()
                return swipe,wait,bottom
            if "--refresh" in sys.argv:
                original=list(range(1001,1031))
                overlap=list(range(1001,1029))+[2001,2002]
                HomeHandler.recommendation_batches=[original,overlap,overlap,overlap]
                HomeHandler.recommendation_requests=0
                HomeHandler.recommendation_wait=HomeHandler.recommendation_fail=0
                def refresh(click,event,wait_for,log_path,process,escape):
                    swipe,wait,bottom=helpers(click,event,log_path)
                    wait_for("NETEASE_HOME avatar ready", 30 if extra else 4)
                    wait("added=30 total=30",lambda:click(128,101))
                    # The API returns 30 songs; only two are new. Keep 28 old rows.
                    wait("added=2 total=30",lambda:click(572,38))
                    assert HomeHandler.recommendation_requests==3
                    log=log_path.read_text()
                    assert "raw=30 valid=30 new=2 staged=2" in log,log
                    assert "new=2 retained=28 total=30" in log,log
                    # An unchanged response must retain the full list without more requests.
                    wait("added=0 total=30",lambda:click(572,38))
                    assert HomeHandler.recommendation_requests==4
                    wait("NETEASE_HOME playlists: 20",lambda:click(360,101))
                    wait("NETEASE_HOME recommendations: 30",lambda:click(128,101))
                    assert HomeHandler.recommendation_requests==5
                    HomeHandler.resolve_id=2001
                    wait("NETEASE_GUEST selected: 2001",lambda:click(240,261))
                    click(650,40);click(50,40)
                    partial=original[:28]+[3001,3002]
                    following=original[:28]+[4001,4002]
                    HomeHandler.recommendation_batches=[original,overlap,overlap,overlap,overlap,partial,partial,following,following]
                    wait("added=2 total=30",lambda:click(572,38))
                    bottom(lambda:HomeHandler.recommendation_requests>=9)
                    wait_for("added=2 total=32")
                    assert "append=1 new=2 retained=0 total=32" in log_path.read_text()
                HomeHandler.playlist_page=0
                scenario("refresh",refresh,36 if extra else 24)
                assert not LoginHandler.failures,LoginHandler.failures
                print("PASS overlapping recommendation refresh retains 30 rows, unchanged refresh and tab navigation preserve recommendations, partial append only adds new songs")
                return
            HomeHandler.recommendation_batches=[list(range(1001,1031)),list(range(1021,1051)),list(range(1041,1071)),list(range(1001,1031)),list(range(2001,2031))]
            HomeHandler.recommendation_requests=0;HomeHandler.recommendation_wait=2
            HomeHandler.recommendation_started.clear();HomeHandler.release_recommendation.clear()
            def append(click,event,wait_for,log_path,process,escape):
                swipe,wait,bottom=helpers(click,event,log_path)
                wait_for("NETEASE_HOME avatar ready", 30 if extra else 4)
                HomeHandler.playlist_page=1
                bottom(lambda:"added=3 total=23" in log_path.read_text())
                wait("NETEASE_HOME recommendations: 30",lambda:click(128,101))
                HomeHandler.resolve_id=1001
                wait("Audio playing:",lambda:click(240,261));wait("已暂停",lambda:click(360,326));click(50,40)
                bottom(lambda:HomeHandler.recommendation_started.is_set())
                assert HomeHandler.recommendation_requests==2
                swipe();swipe()
                assert HomeHandler.recommendation_requests==2,"concurrent load-more requests"
                wait("added=30 total=60",HomeHandler.release_recommendation.set)
                assert "NETEASE_FEED queue extended: 60" in log_path.read_text(),"active queue did not extend"
                # 30 old rows minus a 356px viewport -> unchanged offset 1864.
                HomeHandler.resolve_id=1026
                wait("NETEASE_GUEST selected: 1026",lambda:click(240,261))
                click(650,40);click(50,40)
                bottom(lambda:HomeHandler.recommendation_requests>=4)
                wait_for("added=0 total=60")
                for _ in range(3):swipe()
                assert HomeHandler.recommendation_requests==4,"duplicate-only batches caused a request loop"
                wait("added=30 total=30",lambda:click(572,38))
                HomeHandler.resolve_id=2001
                wait("NETEASE_GUEST selected: 2001",lambda:click(240,261))
                click(650,40);click(50,40)
            scenario("append",append,40 if extra else 32)
            HomeHandler.playlist_page=0;HomeHandler.recommendation_requests=0
            HomeHandler.recommendation_wait=0;HomeHandler.recommendation_fail=2
            HomeHandler.recommendation_batches=[list(range(1001,1031)),list(range(1031,1061)),list(range(1031,1061))]
            def retry(click,event,wait_for,log_path,process,escape):
                swipe,wait,bottom=helpers(click,event,log_path)
                wait_for("NETEASE_HOME avatar ready", 30 if extra else 4)
                wait("NETEASE_HOME recommendations: 30",lambda:click(128,101))
                bottom(lambda:HomeHandler.recommendation_requests>=2)
                wait_for("fixture retry")
                assert HomeHandler.recommendation_requests==2
                wait("added=30 total=60",swipe)
                assert HomeHandler.recommendation_requests==3,"retry skipped or duplicated a batch"
                # Search and playlist pages also append without losing previous rows.
                HomeHandler.search_batches=[list(range(5001,5021)),list(range(5021,5041))]
                HomeHandler.search_requests=0
                wait("NETEASE_GUEST search: 20 tracks",lambda:click(592,101))
                bottom(lambda:HomeHandler.search_requests==2)
                wait_for("NETEASE_GUEST search: 40 tracks")
                wait("NETEASE_HOME playlists: 20",lambda:click(360,101))
                HomeHandler.song_page=0
                wait("NETEASE_HOME tracks: 20",lambda:click(240,353))
                HomeHandler.song_page=1
                bottom(lambda:"added=5 total=25" in log_path.read_text())
            scenario("retry",retry,28 if extra else 18)
            assert not LoginHandler.failures,LoginHandler.failures
    finally:
        HomeHandler.release_recommendation.set();server.shutdown();server.server_close();thread.join()
    print("PASS bottom-triggered append, playlist continuation, fresh 30-song batches, overlap dedup/fill, viewport/queue preservation, repeat exhaustion and retry")
if __name__=="__main__":main()
