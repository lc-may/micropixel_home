#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare an Orion Host image for the primary firmware slot on Linux."""

import argparse
import os
from pathlib import Path
import runpy
import shutil
import subprocess
from tempfile import TemporaryDirectory
import tomllib


BOARD = Path(__file__).resolve().parents[1] / "boards/bl618dg/orion"
HOST_BINARY = "micropixel_orion_host_bl618dg_ap.bin"


def certificate_bundle(image, descriptors):
    entry = descriptors.find_descriptor(descriptors.read_descriptors(image), "CERTS")
    if entry is None:
        return None
    start, end = entry["start_addr"], entry["end_addr"]
    if not 0 < start < end <= len(image) or end - start < 2 or image[start:start + 2] == b"\0\0":
        raise ValueError("Invalid CERTS descriptor in Orion firmware")
    return image[start:end]


def prepare(build_dir, output, sdk):
    source = build_dir / "build_out" / HOST_BINARY
    if output == source.resolve():
        raise ValueError("Use a separate path for the flash image")
    config = (build_dir / "generated/autoconfig.h").read_text().splitlines()
    embeds_certs = "#define CONFIG_X509_CERTIFICATE_BUNDLE 1" in config
    if "#define CONFIG_FHOST 1" in config and not embeds_certs:
        raise ValueError("Orion Wi-Fi Host requires the SDK X.509 certificate bundle")

    expected_certs = None
    if embeds_certs:
        descriptors = runpy.run_path(str(sdk / "tools/byai/multi_bins.py"))["CustomBinManager"]
        expected_certs = certificate_bundle(source.read_bytes(), descriptors)
        if expected_certs is None:
            subprocess.run(["cmake", "--build", str(build_dir), "--target", "post_build"], check=True)
            expected_certs = certificate_bundle(source.read_bytes(), descriptors)
        if expected_certs is None:
            raise ValueError("SDK post_build produced an Orion image without CERTS")

    partitions = tomllib.loads((BOARD / "config/partition_cfg_8M.toml").read_text())["pt_entry"]
    fw_size = next(partition["size0"] for partition in partitions if partition["name"] == "FW")
    tool = sdk / "tools/bflb_tools/bflb_fw_post_proc/bflb_fw_post_proc-ubuntu"
    dts = sdk / "bsp/board/bl618dgdk/config/bl_factory_params_IoTKitA_auto.dts"
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="orion-image-", dir=output.parent) as directory:
        staged = Path(directory) / HOST_BINARY
        shutil.copyfile(source, staged)
        subprocess.run([str(tool), "--chipname=bl618dg", f"--imgfile={staged}", "--appkeys=shared", f"--dtsfile={dts}"],
                       cwd=directory, check=True)
        image = staged.read_bytes()
        if not 0 < len(image) <= fw_size:
            raise ValueError("Prepared Orion image exceeds the primary firmware slot")
        if embeds_certs and certificate_bundle(image, descriptors) != expected_certs:
            raise ValueError("Orion flash image lost CERTS during SDK post-processing")
        staged.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="Existing Orion Host CMake build directory")
    parser.add_argument("output", type=Path, help="Prepared flash image")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("BL_SDK_BASE"), help="Verified Bouffalo SDK checkout")
    args = parser.parse_args()
    if args.sdk is None:
        parser.error("set BL_SDK_BASE or provide --sdk")
    sdk = args.sdk.resolve()
    os.environ["BL_SDK_BASE"] = str(sdk)
    try:
        prepare(args.build_dir.resolve(), args.output.resolve(), sdk)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Unable to prepare Orion image: {error}\n")
    print(f"Orion flash image ready: {args.output}")


if __name__ == "__main__":
    main()
