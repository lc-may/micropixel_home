# MicroPixel

[English](README.md) | [简体中文](README.zh-CN.md)

[MicroPixel](https://micropixel.ai) runs WebAssembly apps on Espressif microcontrollers.
Apps use a C++23 SDK for graphics, input, audio, storage, and sensors, without depending on a board-specific SDK.
The firmware manages hardware, app isolation, and the system UI.

This repository also includes a Linux/WSL2 PC Host for developing and running Guest apps without MCU hardware.

## PC features

| Feature | Available functionality |
|---|---|
| Display and input | LVGL + SDL window, mouse-driven touch and keyboard input, Chinese fonts, and the existing Scene/Raster/Direct Surface services. |
| Home launcher | Launch apps, open the market, return to the current app, and control playback and system volume. Music keeps playing and advances to the next track while Home or the market is visible. |
| Official app market | Browse, search, and view releases; download, validate, and install supported official RV32 AOT bundles, then execute them with QEMU. Portable Wasm runs directly in the native WAMR interpreter. |
| NetEase Cloud Music Guest | Direct connection without an API gateway; official website WeChat QR login, account profile and playlists, daily recommendations, search, favorites, scrolling lists, and playback controls. Full playback or previews depend on the server's permissions. |
| Network and audio | POSIX sockets and mbedTLS, asynchronous Guest HTTP/HTTPS, Host MQTT/MQTTS diagnostics, and local/network MP3, WAV, and Ogg/Opus playback through SDL stereo audio. |
| Guest UI SDK | IconButton, ListItem, ScrollView, and Slider widgets, plus PNG/JPEG texture decoding for downloaded images. |

The current PC Host runs one Guest at a time. Returning to Home preserves that session; launching another
Guest stops the previous one. Multi-Guest AppManager, Activity/Service, and Home widgets are described in
the [architecture proposal](docs/design/app-manager.zh-CN.html), and are not implemented yet.
The new HTTP and URL audio adapters currently target PC; MCU support requires platform adapters.

## Run on a PC

Use Ubuntu 22.04 or WSL2 with GUI/audio support. Install WASI SDK, CMake, Ninja, Python 3, and the SDL2,
libpng, and cJSON development packages; see the [PC setup guide](firmware/pc/README.zh-CN.md) (Chinese).
From the repository root:

```sh
bash tools/pc.sh deps
bash tools/pc.sh configure
bash tools/pc.sh build
bash tools/pc.sh run --home

# Open the official market or the music login screen.
bash tools/pc.sh run --market
bash tools/pc.sh run --music-login
```

`--home` and the music commands automatically build the music Wasm Guest. Press `Home` or `F1` to return
to Home while keeping it running; press `Esc` in the foreground Guest to stop it.
Official RV32 AOT apps additionally require `qemu-system-riscv32` and a bare-metal GCC with
`rv32imafc/ilp32f` Newlib support. Run `bash tools/pc.sh rv32-build` before installing them from the market.
Xtensa AOT and Wi-Fi hardware emulation are not supported. Audio files are downloaded before playback.

See [PC Home](docs/design/pc-home.zh-CN.md), [RV32 execution](docs/design/pc-rv32.zh-CN.md),
and the [music Guest guide](guest/apps/netease-music/README.zh-CN.md) for details (Chinese).

## Hardware

| Chip | Board |
|---|---|
| ESP32-P4 | [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4) |
| ESP32-P4 | [KSDIY ESP32-P4C5 4.3"](https://github.com/kevincoooool/ESP32P4_KSDIY) |
| ESP32-S31 | [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) |
| ESP32-S3 | [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) |
| ESP32-S3 | [LCKFB SZPI](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html) |
| ESP32-S3 | [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) |
| ESP32-S3 | [SenseCAP Watcher](https://wiki.seeedstudio.com/cn/getting_started_with_watcher/) |

The Host uses ESP-IDF 6.1 and a pinned [WAMR fork](https://github.com/78/wasm-micro-runtime).
MCU apps are compiled to architecture-specific AOT v6 bundles. The ABI is still evolving.

## Build an app

Install the [SDK](https://micropixel.ai/docs/environment/) and connect a device running MicroPixel firmware.
App development does not require ESP-IDF.

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
micropixel --transport usb run
```

`run` builds, installs, starts the app, and follows its logs. Ctrl-C stops log streaming; the app keeps running.
See the [quickstart](guest/sdk/QUICKSTART.md) and [publishing guide](guest/sdk/PUBLISHING.md).

## Build the firmware

Initialize submodules, activate ESP-IDF 6.1, and configure WASI SDK and the matching MicroPixel WAMRC.
See the [build guide](docs/development/flashing.zh-CN.md) (Chinese) for toolchain setup and flashing.

```sh
git submodule update --init --recursive
python3 -m pip install -r requirements-dev.txt
bash tools/p4.sh build-host
```

Other profiles: `bash tools/s31.sh build-host`, `bash tools/s3.sh build-host <box3|szpi|cores3|watcher>` and
`python3 tools/firmware.py <ksdiy-p4c5|ksdiy-p4c5-landscape> build`.

## Project

- [Guest SDK](guest/sdk/README.md) — app APIs and examples.
- [Firmware](firmware/README.md) — Host runtime and board support.
- [Documentation](docs/README.md) — architecture, protocols, and development guides.
- [Contributing](CONTRIBUTING.md).

## License

Project-authored code, documentation, and assets use [Apache-2.0](LICENSE), unless stated otherwise.
See [third-party notices](THIRD_PARTY_NOTICES.md) for dependency licenses and exceptions.
