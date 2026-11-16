#!/usr/bin/env python3
"""Package conformance sources with explicit requirements for the RV32 runner."""
import json
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]


def main():
    for name, app_id, components, services in (
        ("cold_endpoint", "micropixel.cold.receiver", {"activity": False, "service": True, "background": ["task"], "widgets": 0}, {"lifecycle": 65536, "message": 65537, "job": 65536}),
        ("app_services_contract", "test.app-services-contract", {"activity": True, "service": True, "background": ["task"], "widgets": 1}, {"lifecycle": 65536, "widget": 65536, "message": 65536}),
    ):
        project = ROOT / "build/pc-rv32-fixtures" / name
        project.mkdir(parents=True, exist_ok=True)
        # The source is shared with native conformance tests; only packaging differs.
        (project / "main.cpp").write_text('#include "' + str(ROOT / f"guest/tests/conformance/{name}.cpp") + '"\n')
        manifest = {
            "schema_version": 1, "app_id": app_id, "version": "0.1.0", "threading": "none",
            "title": name, "sources": ["main.cpp"],
            "requirements": {
                "schema_version": 2,
                "display": {"layouts": ["square"], "min_width": 240, "min_height": 240},
                "required": [], "optional": [], "any_of": [],
                "services": services, "components": components,
            },
        }
        (project / "app.json").write_text(json.dumps(manifest))
        output = ROOT / "build/pc-rv32-apps" / name
        subprocess.run(
            [sys.executable, "tools/pc.py", str(project), "--backend", "rv32", "--output-dir", str(output)],
            cwd=ROOT, check=True,
        )
        generated = output / (app_id.rsplit(".", 1)[-1] + ".pc.json")
        (output / f"{name}.pc.json").write_bytes(generated.read_bytes())


if __name__ == "__main__":
    main()
