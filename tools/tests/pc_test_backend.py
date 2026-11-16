"""Shared paths for the same business tests on native Wasm and RV32 AOT."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]


def app_path(name, backend=None):
    backend = backend or ("rv32" if "--rv32" in sys.argv else "native")
    folder = "pc-rv32-apps" if backend == "rv32" else "pc-apps"
    path = ROOT / f"build/{folder}/{name}/{name}.pc.json"
    assert path.is_file(), f"Build {name} for {backend} first"
    return path
