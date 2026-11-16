#!/usr/bin/env python3
"""Actual Wasm weather work, with success/failure/deadline responses."""
import http.server,json,os,subprocess,tempfile,threading,time
from pc_test_backend import app_path
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class Handler(http.server.BaseHTTPRequestHandler):
    count=0
    def log_message(self,*args):pass
    def do_GET(self):
        Handler.count+=1;n=Handler.count
        if n==2:self.send_response(503);self.end_headers();return
        if n==3:time.sleep(4)
        data=json.dumps({"current":{"time":int(time.time()),"temperature_2m":-0.5 if n==1 else 22.5,"weather_code":1}}).encode()
        self.send_response(200);self.send_header("Content-Length",str(len(data)));self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass

def main():
    server=http.server.ThreadingHTTPServer(("127.0.0.1",0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    results=ROOT/"build/pc-jobs-tests";results.mkdir(exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="jobs-",dir=results) as directory:
            env=os.environ.copy();env["MICROPIXEL_PC_DATA_DIR"]=directory
            with (results/"jobs.log").open("w") as log:
                r=subprocess.run([str(ROOT/"build/pc/pc-app-scheduler-test"),str(app_path("weather")),str(app_path("components-demo")),f"http://127.0.0.1:{server.server_port}/weather",str(app_path("cold_endpoint"))],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60)
            assert r.returncode==0,(results/"jobs.log").read_text()[-10000:]
            assert Handler.count==4,Handler.count
        print("PASS actual Wasm weather background jobs and durable stop/refresh policies")
    finally:server.shutdown();server.server_close()
if __name__=="__main__":main()
