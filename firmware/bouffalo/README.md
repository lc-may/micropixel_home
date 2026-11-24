# Bouffalo build foundation

This directory provides external SDK build profiles for BL616CL and BL618DG B0 AP,
including the BL618DG Orion board.
Each profile builds a minimal FreeRTOS/WAMR validation executable. Full MicroPixel
firmware composition and board display, input, audio and storage adapters are
separate follow-ups. See [runtime contracts](docs/runtime.md).

## Build

Use Linux x86_64 or WSL, Python 3.12+, Git, Make and CMake 3.20+.
Follow [SDK setup](docs/sdk.md) for pinned source and verified toolchains, then:

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --directory ../micropixel-bouffalo-sdk
export BL_SDK_BASE="$(cd ../micropixel-bouffalo-sdk && pwd)"
# Add the verified Zephyr and Bouffalo toolchain bin directories to PATH first.
python3 firmware/bouffalo/tools/prepare_deps.py
# Build the pinned wamrc and add it to PATH as described in docs/runtime.md.
make -C firmware/bouffalo/boards/bl616cl
make -C firmware/bouffalo/boards/bl618dg
```

For Orion, use its [Gitee SDK revision](docs/sdk.md#bl618dg-orion) with the same
toolchains and WAMR dependency. From the MicroPixel repository root:

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --board bl618dg_orion --directory ../bouffalo_sdk
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion \
    BL_SDK_BASE="$(cd ../bouffalo_sdk && pwd)"
```

The Orion build defaults to the sibling `bouffalo_sdk` checkout and resolves
`bsp/board/bl618dg_orion` in that SDK. Set `BL_SDK_BASE` to select a different
checkout of the Orion revision. The Orion output directory is `build-orion/`.

`cmake/sdk.cmake` verifies SDK provenance before configuring each target. All
use RV32 ILP32F, C++23, disabled exceptions and RTTI. Unused prebuilt multimedia
and PKA libraries are excluded because the SDK's DG variants use ILP32D. The
linker continues to enforce ABI compatibility. Outputs stay in each board's
ignored build directory. `CONFIG_SKIP_COMBINE` produces raw test images without
installation partition layouts. Use `BUILD_DIR=build-other` for a fresh output directory.

## Source dependencies

`dependencies.json` pins the same WAMR revision/AOT v6 as the Espressif submodule.
The separate ignored source directory lets the Bouffalo build work without an
ESP-IDF component checkout. The submodule revision is unchanged.

```sh
python3 firmware/bouffalo/tools/prepare_deps.py
python3 firmware/bouffalo/tools/prepare_deps.py --check
```

Downloads are checksum-verified before extraction and installation. Existing
source trees are checked against the manifest's file fingerprints and are never
overwritten. That check covers the listed critical files, not every local file;
use a freshly prepared tree for release builds. No dependencies or toolchain
binaries are committed.

## Checks

```sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/tests/test_firmware_host.sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/check_firmware_style.sh --format-only
git diff --check
```

The tests use temporary local Git repositories and source archives without
network access. They cover repeat preparation, wrong revision, source changes,
untracked files, damaged patches, competing CMake SDK paths, CI board selection,
failed dependency checksum rejection, and byte-stack conversion at the native
16/32-bit depth boundaries. The platform selector leaves the existing Espressif
checks as the default.
