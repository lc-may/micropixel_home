#!/usr/bin/env python3
"""The common Widget/Message/Lifecycle wire contract through the actual RV32 bridge."""
import os
import subprocess
import tempfile
from pc_test_backend import ROOT, app_path

def main():
    results = ROOT / "build/pc-rv32-service-tests"
    results.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=results) as directory:
        env = dict(os.environ, MICROPIXEL_PC_DATA_DIR=directory)
        for bad in (False, True):
            args = [str(ROOT / "build/pc/micropixel-pc"), "--headless", "--autostart", "--exit-on-app-complete", "--seconds", "30"]
            if bad: args += ["--guest-arg", "--bad-pointer"]
            args.append(str(app_path("app_services_contract", "rv32")))
            result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=35)
            log = result.stderr.decode(errors="replace")
            (results / ("bounds.log" if bad else "contract.log")).write_text(log)
            assert result.returncode == int(bad), log
            assert ("out of bounds memory access" if bad else "APP SERVICES CONTRACT PASS") in log, log
    print("PASS RV32 Lifecycle/Widget/Message wire contract, ticket identity, duplicate/stale rejection and checked-memory trap")

if __name__ == "__main__":
    main()
