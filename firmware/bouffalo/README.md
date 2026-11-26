# MicroPixel for Bouffalo

MicroPixel runs WebAssembly Guests on the Bouffalo SDK using FreeRTOS and WAMR.
The BL618DG Orion Host integrates a 480 × 480 touchscreen, audio, Wi-Fi and an
embedded app catalog. The development-kit profiles provide smaller firmware
images for validating the runtime and toolchain.

## Boards and build profiles

| Board | Build selection | Firmware |
| --- | --- | --- |
| BL616CL DK | [`boards/bl616cl`](boards/bl616cl/) | FreeRTOS/WAMR validation with RV32 ILP32F AOT |
| BL618DG DK, AP | [`boards/bl618dg`](boards/bl618dg/) with its default `BOARD=bl618dgdk` | FreeRTOS/WAMR validation with RV32 ILP32F AOT |
| BL618DG Orion, B1 silicon, AP | `BOARD=bl618dg_orion` | Runtime validation by default. Add `HOST_PORT=1` for the full Host, `AUDIO_TEST=1` for the independent audio Guest, or `DISPLAY_CONFIG` for panel validation. |

The DK profiles use the pinned public SDK. Orion uses a separate pinned Gitee
revision with `bsp/board/bl618dg_orion`. The Orion board selects
`CPU_MODEL=b1`. The full Orion Host uses the SDK's
RV32 ILP32D ABI, including for its default AOT catalog. The independent audio
test uses ILP32F. The display validation ABI depends on the panel mode.
[SDK profiles](docs/sdk.md) · [Runtime validation](docs/runtime.md#validation-executable) · [Orion display profiles](docs/orion.zh-CN.md#单独验证显示驱动)

## Orion Host

The default `HOST_PORT=1` profile enables Wi-Fi and AOT. The Home screen launches
the independent Settings Guest, Clock and Alarm, Weather, NetEase Music and
News. The catalog also includes four diagnostic Guests.
[Catalog and Host composition](boards/bl618dg/orion/firmware_app.cpp)

| Area | Integrated behavior |
| --- | --- |
| Guest execution | Four Session slots share one WAMR runtime. Each active Session has its own module instance, linear memory, event queue and resource handles. Touch input goes to the foreground Session. Background Services can continue to update Home widgets. |
| Display and input | ST7102 MIPI DSI panel, ST7123 touch, LVGL 9, retained Scene rendering, text input, dynamic textures and JPEG/PNG decoding. The Back control navigates to the previous page. A swipe from the bottom center returns to Home. |
| Audio | JY6316 through SDK `xcodec_dev`, with tone synthesis and PCM streaming. A Host worker fetches and incrementally decodes MP3 over HTTPS for NetEase Music. Playback can continue while its Service runs in the background. |
| Wi-Fi and Guest networking | The Settings Guest handles scans and connection. LittleFS remembers the last successful network for reconnect. DHCP obtains an address at connection time. Static IPv4 settings are persisted when static mode is selected. The Guest Network Service provides HTTPS GET, POST and response metadata. |
| Clock and persistent data | The Host refreshes its UTC reference through SNTP every 15 minutes after synchronization. LittleFS stores alarm schedules, the latest weather result and selected app settings. Weather and music Sessions publish their state to Home. |

The production profile runs on the AP core with 16 MiB of PSRAM. The SDK copies
regular Host code to PSRAM, and the runtime loads Guest code and linear memory
there. AOT Guest stop is cooperative. `ORION_AOT=0` builds an interpreter with an
instruction budget and embeds Wasm in the firmware image.
[Runtime contracts](docs/runtime.md) · [Memory and startup](docs/orion.zh-CN.md#内存与启动性能)

The catalog is fixed at build time. Guest storage is available for selected
applications and keys. On-device Bundle installation, general-purpose Guest
KV storage and the Espressif OTA workflow are outside the current Orion Host
feature set. `RADIO=ble` and `RADIO=dual` are experimental profiles. The default
Wi-Fi profile focuses on Wi-Fi STA and lwIP.
[Wireless profiles](docs/orion.zh-CN.md#wi-fi-与-ble) · [Orion Host guide](docs/orion.zh-CN.md#pc-host-移植)

## Source layout

| Directory | Responsibility |
| --- | --- |
| [`boards/`](boards/) | Board build entry points and composition. [Orion](boards/bl618dg/orion/) organizes audio, display, Home, RF, diagnostics, fonts and configuration in board-local directories. |
| [`host/`](host/) | Product state and coordination, including alarm storage, Home widgets and per-Session HTTP requests. |
| [`runtime/`](runtime/) | Guest Session execution, ABI endpoints, graphics submission and the [WAMR port](runtime/wamr/). |
| [`platform/`](platform/) | SDK adapters for audio, graphics, memory, networking, wireless and LittleFS. [`platform/include/`](platform/include/) supplies Bouffalo headers used when compiling shared Host code. |
| [`work/`](work/) | FreeRTOS task creation, stack size conversion and background task helpers. |
| [`config/`](config/), [`cmake/`](cmake/), [`examples/`](examples/), [`tools/`](tools/) | Kernel configuration, build wiring, validation firmware and build/test utilities. |

### Where firmware/common comes from

[`firmware/common/`](../common/) was extracted from `firmware/espressif/main/`
when Bouffalo's dependency on that source tree was removed. It is now the
shared implementation layer for Bouffalo, Espressif and PC. Each build selects
the modules it uses.

The shared layer contains Device contracts, `AppManager`, `AppHub`, lifecycle
and event handling, the service registry, PCM service, and software graphics
and audio code. It includes both interfaces and implementations. Some
implementations select target-specific behavior at compile time, such as audio
task creation and LVGL font locking. Bouffalo supplies its native timer,
allocation, logging and audio status adapters under
[`platform/include/`](platform/include/). Board wiring, RF behavior and the
Orion UI belong to this port's board layer.

The dependency direction is from each target to `firmware/common/`. A Bouffalo
checkout can omit the entire `firmware/espressif/` tree. Guest applications,
the SDK and the wire ABI live separately in [`guest/`](../../guest/).
[Shared Host source selection](cmake/sdk.cmake) · [Orion build wiring](cmake/host_port.cmake)

## Build

Run the commands from the repository root on Linux x86_64 or WSL with
Python 3.12+, Git, Make and CMake 3.20+. Prepare the two RISC-V toolchains in
[SDK setup](docs/sdk.md#toolchains) and put their executables on `PATH`.

### Prepare dependencies

```sh
python3 firmware/bouffalo/tools/prepare_deps.py
python3 firmware/bouffalo/tools/prepare_deps.py --check
```

[`dependencies.json`](dependencies.json) pins the WAMR archive and AOT format.
Preparation verifies the archive and installs the source under the ignored
`third_party/` directory. Existing sources are checked against the manifest's
critical-file fingerprints. Prepare `wamrc 2.4.3` with the matching
[WAMR build instructions](docs/runtime.md#validation-executable) and put it on
`PATH`. Runtime validation also uses `clang-18` and `wasm-ld-18`.

### Build Orion Host

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --board bl618dg_orion --directory ../bouffalo_sdk
export BL_SDK_BASE="$(cd ../bouffalo_sdk && pwd)"
export WASI_SDK_PATH=/path/to/wasi-sdk

make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion HOST_PORT=1
```

Orion resolves the sibling `bouffalo_sdk` by default. Use `BL_SDK_BASE` for a
different verified checkout. CMake checks SDK provenance before configuration.
The Host builds its Guests with the WASI SDK and generates its extended font
using Pillow and fontTools. Supply `ORION_FONT_SOURCE=/path/to/SourceHanSansSC-Regular.otf`
to use the pinned font from a local file.

| Option | Purpose |
| --- | --- |
| `ORION_AOT=1` | Default production AOT catalog. Set `0` for the interpreter profile. |
| `RADIO=wifi` | Default Wi-Fi profile. The board guide describes the experimental BLE and dual-radio profiles. |
| `AUDIO_TEST=1` | Independent audio validation firmware. Select this as its own profile. |
| `DISPLAY_CONFIG=/path/to/panel.defconfig` | Independent display validation. Some panel buses also require `DISPLAY_GPIO_SOURCE`. |
| `LCD_CONFIG=/path/to/lcd_conf_user.h` | Override the panel configuration for the full Host. |
| `BUILD_DIR=build-other` | Use a separate build directory when changing profile options. |

The default AOT Host produces
`firmware/bouffalo/boards/bl618dg/build-orion-host-wifi/build_out/micropixel_orion_host_bl618dg_ap.bin`
and `orion_apps.bin` at the root of the same build directory. The default
runtime validation and standalone audio build use `build-orion/` and
`build-orion-audio/` respectively. SDK profiles and exact pins are listed in
[`sdk-source.json`](sdk-source.json).

### Build the runtime and peripheral validation firmware

Use the Orion SDK selection above for these Orion targets.

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion AUDIO_TEST=1
```

Use the public SDK selection for the two development kits.

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --directory ../micropixel-bouffalo-sdk
export BL_SDK_BASE="$(cd ../micropixel-bouffalo-sdk && pwd)"

make -C firmware/bouffalo/boards/bl616cl
make -C firmware/bouffalo/boards/bl618dg
```

## Orion Flash and UART

The [8 MiB partition layout](boards/bl618dg/orion/config/partition_cfg_8M.toml)
reserves two 3 MiB firmware slots, 868 KiB for image resources, 896 KiB for the
AOT catalog and 128 KiB for LittleFS. Boot and factory data remain in reserved
regions.
[Partition map and migration](docs/orion.zh-CN.md#8-mib-flash-分区)

The build emits a raw firmware image. Before flashing, verify the board and
partition map, and post-process a copy of the image with the SDK and the
appropriate RF parameters. The AOT Host checks the catalog against a digest
embedded in the firmware. Use the matched firmware and catalog from the same
build when updating either to a version with a different catalog digest.
[Orion flashing procedure](docs/orion.zh-CN.md#pc-host-移植)

The Orion console runs at 2 Mbps. Typical Host commands are shown below.

```text
mpx list
mpx status
mpx perf
mpx run weather
mpx back
mpx home
mpx stop weather
```

`mpx status` reports LCD and audio counters, and `mpx perf` reports heaps and
task stack watermarks. The [frame capture guide](docs/orion.zh-CN.md#屏幕)
describes grabbing the RGB565 framebuffer over UART.
[More console operations](docs/orion.zh-CN.md#pc-host-移植)

## Checks

```sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/tests/test_firmware_host.sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/check_firmware_style.sh --format-only
git diff --check
```

The Host tests cover SDK provenance, dependency checksums, the source-tree
boundary, alarm persistence, MP3 input fragmentation and task stack depth
conversion. For board validation, the runtime image reports WAMR execution and
memory checks over UART. The standalone audio image runs a 1 kHz tone and PCM
Guest, and the Orion Host logs its AOT catalog verification and each Guest's
execution backend. The [runtime smoke guide](docs/runtime.md#validation-executable)
and [audio test guide](docs/orion.zh-CN.md#音频) describe their expected output.

See [third-party notices](../../THIRD_PARTY_NOTICES.md) for dependency attribution.
