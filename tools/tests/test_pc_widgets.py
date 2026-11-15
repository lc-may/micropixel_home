#!/usr/bin/env python3
"""Actual Guest widget gestures, independent hit regions, and pending actions."""
import os
from pathlib import Path
import tempfile
import time
from test_pc_market import ROOT, navigate_x11

def main():
    with tempfile.TemporaryDirectory(prefix="pc-widgets-",dir=ROOT/"build") as temporary:
        output=Path(temporary)
        env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=str(output/"data"))
        def drive(click,event,wait_for,log_path,process,escape):
            wait_for("WIDGETS ready")
            event(6,0,40,44);event(4,1,40,44);event(6,0,400,44);event(5,1,400,44)
            wait_for("WIDGET slider commits: 1")
            # A second finger-down preview is cancelled before release.
            event(4,1,40,44);event(6,0,160,44);event.key(0xff0d);event(5,1,160,44)
            wait_for("WIDGET slider cancelled value:")
            event(6,0,100,280);event(4,1,100,280);event(6,0,100,160);event(5,1,100,160)
            wait_for("WIDGET scroll offset: 120")
            log=log_path.read_text()
            assert "WIDGET row body:" not in log and "WIDGET row action:" not in log,"scroll generated a click"
            click(140,190);wait_for("WIDGET row body: 1")
            click(392,190);wait_for("WIDGET row action: 1")
            click(532,48);click(532,48)
            wait_for("WIDGET toggle: 1")
            log=log_path.read_text()
            assert "WIDGET slider commits: 2" not in log,"cancelled slider committed"
            assert "WIDGET row body: 2" not in log,"trailing action bubbled into row"
            assert "WIDGET toggle: 2" not in log,"pending toggle accepted a duplicate"
        navigate_x11(env,output,["--autostart","build/pc-apps/music_widgets/music_widgets.pc.json"],drive,seconds=8)
        dest=ROOT/"build/pc-widget-tests";dest.mkdir(exist_ok=True)
        (dest/"widgets.log").write_text((output/"navigation.log").read_text())
        (dest/"widgets.png").write_bytes((output/"navigation.png").read_bytes())
    print("PASS SDK slider range/drag/cancel, scroll-versus-tap, independent row actions and pending toggle")
if __name__=="__main__":main()
