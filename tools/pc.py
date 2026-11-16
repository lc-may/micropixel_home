#!/usr/bin/env python3
"""Build native Wasm or RV32 AOT Guests for the Linux PC Host."""
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
    parser.add_argument("--profile", choices=("development", "release"))
    parser.add_argument("--backend", choices=("native", "rv32"), default="native")
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
    folder = "build/pc-rv32-apps" if args.backend == "rv32" else "build/pc-apps"
    output = (args.output_dir or ROOT / folder / name).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("WASI_SDK_PATH") and Path("/opt/wasi-sdk/bin/clang++").is_file():
        os.environ["WASI_SDK_PATH"] = "/opt/wasi-sdk"
    if args.backend == "rv32":
        if not project:
            parser.error("RV32 packaging requires an app.json project")
        if args.extra_source:
            parser.error("For RV32, add extra sources to the project's app.json")
        if not os.environ.get("WAMRC"):
            for candidate in (
                "build/tools/wamrc-2.4.3-riscv32-ilp32f",
                "build/tools/wamrc-pc-llvm15/wamrc-2.4.3",
                "build/tools/wamrc-pc-llvm14/wamrc-2.4.3",
            ):
                if (ROOT / candidate).is_file():
                    os.environ["WAMRC"] = str(ROOT / candidate)
                    break
        bundle = sdk.package_project(
            project, args.profile or "release", output, None, "riscv32-ilp32f", False, args.force
        )
        pack = None
    else:
        generated, pack = sdk.prepare_project(project, output) if project else (None, None)
        sources = [*(project.sources if project else [source]), *(p.resolve() for p in args.extra_source)]
        sdk._run_build_sources(argparse.Namespace(
            source=str(sources[0]), extra_source=[str(s) for s in sources[1:]], name=name,
            profile=args.profile or "development", aot_target=None, generated_include_override=generated,
            output_dir_override=output, threading="none", unchecked_memory=False,
            force=args.force, wasm_only=True,
        ))
    raw = json.loads(manifest.read_text()) if manifest else {}
    title_data = raw.get("title", name)
    title = title_data if isinstance(title_data, str) else title_data.get("values", {}).get("en", name)
    descriptor = {
        "schema_version": 1, "app_id": raw.get("app_id", f"micropixel.pc.{name}"),
        "title": title, "wasm": bundle.name if args.backend == "rv32" else f"{name}.wasm",
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
