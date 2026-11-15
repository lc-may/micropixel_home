#!/usr/bin/env python3
"""Build portable MicroPixel Guests for the Linux PC Host, without wamrc."""
from __future__ import annotations

import argparse
import importlib.machinery
import importlib.util
import json
import os
import importlib
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def sdk_module():
    loader = importlib.machinery.SourceFileLoader("micropixel_pc_sdk", str(ROOT / "tools/micropixel"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[loader.name] = module
    loader.exec_module(module)
    return module


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Existing app directory, app.json, or a conformance .cpp")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--extra-source", type=Path, action="append", default=[])
    parser.add_argument("--profile", choices=("development", "release"), default="development")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    sdk = sdk_module()
    builder_path = ROOT / "tools/build_app_bundle.py"
    spec = importlib.util.spec_from_file_location("pc_bundle_builder", builder_path)
    builder = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = builder
    spec.loader.exec_module(builder)
    source = args.source.resolve()
    manifest = sdk.resolve_project_manifest(str(source))
    project = sdk.load_project_manifest(manifest) if manifest else None
    if project and project.threading != "none":
        parser.error("The PC Host currently supports the standard single-threaded Guest contract")
    name = project.name if project else source.stem
    output = (args.output_dir or ROOT / "build/pc-apps" / name).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("WASI_SDK_PATH") and Path("/opt/wasi-sdk/bin/clang++").is_file():
        os.environ["WASI_SDK_PATH"] = "/opt/wasi-sdk"
    generated, pack = sdk.prepare_project(project, output) if project else (None, None)
    sources = [*(project.sources if project else [source]), *(p.resolve() for p in args.extra_source)]
    sdk._run_build_sources(argparse.Namespace(
        source=str(sources[0]), extra_source=[str(s) for s in sources[1:]], name=name,
        profile=args.profile, aot_target=None, generated_include_override=generated,
        output_dir_override=output, threading="none", unchecked_memory=False,
        force=args.force, wasm_only=True,
    ))
    raw = json.loads(manifest.read_text()) if manifest else {}
    title_data = raw.get("title", name)
    title = title_data if isinstance(title_data, str) else title_data.get("values", {}).get("en", name)
    descriptor = {
        "schema_version": 1, "app_id": raw.get("app_id", f"micropixel.pc.{name}"),
        "title": title, "wasm": f"{name}.wasm",
        "resources": Path(pack).relative_to(output).as_posix() if pack else "",
    }
    if raw.get("requirements") is not None:
        descriptor["requirements"] = builder.validate_requirements(raw["requirements"])
        descriptor["core_abi"] = builder.CORE_ABI_VERSION
    path = output / f"{name}.pc.json"
    path.write_text(json.dumps(descriptor, ensure_ascii=False, indent=2) + "\n")
    print(f"PC app ready: {path}")


if __name__ == "__main__":
    main()
