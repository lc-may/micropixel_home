#!/usr/bin/env python3
"""Actual Clock Wasm, stopped-app alarm persistence and unchanged media intent."""
import os,subprocess,tempfile,wave,math,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 results=ROOT/"build/pc-alarm-store-tests";results.mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix="data-",dir=results) as directory:
  directory=Path(directory);source=directory/"tone.wav"
  with wave.open(str(source),"wb") as f:
   f.setnchannels(2);f.setsampwidth(2);f.setframerate(48000);cycle=b"".join(struct.pack("<hh",int(6000*math.sin(2*math.pi*440*i/48000)),int(6000*math.sin(2*math.pi*660*i/48000))) for i in range(48000));f.writeframes(cycle*30)
  env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=str(directory/"storage"))
  with (results/"alarms.log").open("w") as log:r=subprocess.run([str(ROOT/"build/pc/pc-alarm-system-test"),str(ROOT/"build/pc-apps/clock-alarm/clock-alarm.pc.json"),str(source)],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=35)
  assert r.returncode==0,(results/"alarms.log").read_text()[-12000:]
 # Exercise the concrete user-facing cleanup commands, with independent fixture storage.
 import sqlite3
 for option in ("--clear-app-data","--uninstall-app"):
  with tempfile.TemporaryDirectory(prefix="cli-",dir=results) as directory:
   env=dict(os.environ,MICROPIXEL_PC_DATA_DIR=directory)
   app=str(ROOT/"build/pc-apps/clock-alarm/clock-alarm.pc.json");binary=str(ROOT/"build/pc/micropixel-pc")
   seed=subprocess.run([binary,"--headless","--autostart","--seconds","1","--guest-arg","--alarm-test",app],env=env,capture_output=True,timeout=10)
   assert seed.returncode==0 and b"CLOCK test alarm registered" in seed.stderr,seed.stderr.decode()
   removed=subprocess.run([binary,"--headless",option,"micropixel.clock-alarm",app],env=env,capture_output=True,timeout=10)
   assert removed.returncode==0 and b"status=0" in removed.stderr,removed.stderr.decode()
   db=sqlite3.connect(Path(directory)/"schedules.sqlite3");assert db.execute("SELECT count(*) FROM alarms").fetchone()[0]==0
   if option=="--uninstall-app":assert db.execute("SELECT value FROM removed WHERE app='micropixel.clock-alarm'").fetchone()[0]==1
   else:assert db.execute("SELECT value FROM stopped WHERE app='micropixel.clock-alarm'").fetchone()[0]==1
   db.close()
 print("PASS actual durable alarm restart, stopped Clock/time, pause and new-generation media during focus, cleanup CLI")
if __name__=="__main__":main()
