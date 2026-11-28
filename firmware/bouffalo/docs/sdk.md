# Reproducible SDK setup

Run the commands from the MicroPixel repository root on Linux x86_64 or WSL.
Use Python 3.12+, Git, Make, CMake and the toolchains below. Serial control also
requires `pyserial` 3.5 in the Python environment that owns the port. The pinned
SDK includes its Kconfig and TOML Python modules. Its packaged Linux firmware
postprocessor requires a compatible glibc host; Ubuntu 24.04 is the tested host.

## Source and patches

The authoritative source lock is [sdk-source.json](../sdk-source.json):

- Repository: <https://github.com/bouffalolab/bouffalo_sdk.git>
- Commit: `eba87edced5a03ce8bcad8cf6002494982f83a5f`
- Public SDK contents are in one Git tree; no private submodule checkout is needed.
```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --directory ../micropixel-bouffalo-sdk
export BL_SDK_BASE="$(cd ../micropixel-bouffalo-sdk && pwd)"
python3 firmware/bouffalo/tools/check_sdk.py --exact
```

Preparation uses a temporary sibling directory and installs it only after verification.
Repeating the command verifies an existing checkout without modifying it. `--exact`
rejects a different HEAD, undeclared tracked changes, untracked files, damaged
patches, or modified critical inputs. This initial foundation declares no SDK
patches. The preparation and verification tools also support explicitly declared,
checksum-pinned patches for future adapters.

## BL618DG Orion

The `bl618dg_orion` board uses the Gitee SDK revision
[`9e34f621`](https://gitee.com/bouffalolab/bouffalo_sdk/commit/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464).
This revision includes Orion's BSP under `bsp/board/bl618dg_orion`, including its
console GPIO and 16 MiB PSRAM configuration. The SDK lock and fingerprints
are recorded under `bl618dg_orion` in the same manifests.
The pinned [Orion Bluetooth RF patch](../patches/orion-bt-standalone-spdt.patch)
selects GPIO16 for standalone Bluetooth and keeps the pin on hardware SPDT
in Wi-Fi/Bluetooth coexistence builds. The [BR/EDR L2CAP patch](../patches/orion-bredr-flush-timeout.patch)
accepts the peer's Flush Timeout option during AVDTP media channel setup.
The [AP exception entry patch](../patches/orion-exception-entry-long-call.patch)
uses a long call from the trap vector so the full Host can link with Classic Bluetooth enabled.
`prepare_sdk.py` applies the declared patches to a new checkout. Apply the missing
patches with `git apply` to an existing checkout of the pinned revision before
running the exact check.

Orion selects `CPU_MODEL=b1` and generates `CPU_MODEL_B1`. In the pinned SDK,
both B0 and B1 use the non-A0 AP compiler path with RV32D and ILP32D. The SDK
changes its link inputs according to the model selector.

| Link input | SDK behavior for B0 | SDK behavior for B1 |
| --- | --- | --- |
| PHY/RF | `libbl618dg_phyrf_b0.a` | `libbl618dg_phyrf.a`. These two AP archives are byte-identical at this SDK revision. |
| Multimedia and PKA | `*_b0.a` archives using ILP32D | Unsuffixed archives using ILP32F. The Orion Host, DSI video validation and BLE peripheral test select the ILP32D archives. |
| C++ standard library | The SDK adds `stdc++` | The MicroPixel build adds `stdc++` explicitly. |

Both AP profiles use `bl618dg_common.ld.in`. The SDK's B0-specific linker
script applies to the LP core. For AP, the SDK suppresses RWX segment warnings
on B0. The B1 build uses the linker's default diagnostics.

[Orion SDK integration](../cmake/sdk.cmake) ·
[SDK PHY selection](https://github.com/bouffalolab/bouffalo_sdk/blob/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/drivers/soc/bl618dg/phyrf/CMakeLists.txt#L2-L13) ·
[SDK multimedia selection](https://github.com/bouffalolab/bouffalo_sdk/blob/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/drivers/lhal/src/multimedia/CMakeLists.txt#L14-L20) ·
[SDK link flags](https://github.com/bouffalolab/bouffalo_sdk/blob/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/cmake/compiler_flags.cmake#L77-L85)

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --board bl618dg_orion --directory ../bouffalo_sdk
python3 firmware/bouffalo/tools/check_sdk.py --board bl618dg_orion --sdk ../bouffalo_sdk --exact
```

## Toolchains

The Linux x86_64 download URLs and SHA256 digests are also in `sdk-source.json`.
Download each archive, compare the complete SHA256 before extracting, and add
the directory containing its compiler executable to `PATH`.

| Purpose | Public release and asset | SHA256 |
| --- | --- | --- |
| Compile/link Host and SDK | [Zephyr SDK v1.0.0-rc1](https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v1.0.0-rc1), `toolchain_gnu_linux-x86_64_riscv64-zephyr-elf.tar.xz` | `9ed3eb07a7c47a49b5a6b87c8fa34c2fc81f71a112c3eca78ca7e88502d18677` |
| SDK DWARF `nm` and `objdump` | [Bouffalo toolchain](https://github.com/bouffalolab/toolchain/releases/tag/default), `gcc-riscv-14.3.0-linux-x86_64.tar.xz` | `93128c0da22cbbcf8473fae8c38b63232ebcd8d51969a14dc8491e77721b63a5` |

The Bouffalo release tag is mutable; the asset digest is the fixed input. If
its contents change, reject the download rather than accepting the new bytes.
Both compilers report GCC 14.3.0; the Host compiler identifies itself as
`Zephyr SDK 1.0.0-rc1`. This port requires the Zephyr C++23 standard library and
Picolibc; putting the Bouffalo toolchain on PATH does not change `CROSS_COMPILE`.

For example, after downloading the Zephyr archive:

```sh
echo '9ed3eb07a7c47a49b5a6b87c8fa34c2fc81f71a112c3eca78ca7e88502d18677  toolchain_gnu_linux-x86_64_riscv64-zephyr-elf.tar.xz' | sha256sum -c -
# Extract each verified archive into its own directory, then set actual bin paths.
export PATH=/path/to/zephyr/bin:/path/to/bouffalo/bin:$PATH
riscv64-zephyr-elf-g++ --version
riscv64-unknown-elf-nm --version
riscv64-unknown-elf-objdump --version
```
