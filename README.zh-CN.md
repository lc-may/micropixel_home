# MicroPixel

[English](README.md) | [简体中文](README.zh-CN.md)

[MicroPixel](https://micropixel.ai) 是 Espressif 微控制器上的 WebAssembly 应用运行时。
应用通过 C++23 SDK 使用图形、输入、音频、存储和传感器，无需依赖板级 SDK。
固件负责硬件管理、应用隔离和系统界面。

本仓库同时提供 Linux/WSL2 PC Host，可在没有 MCU 硬件时开发和运行 Guest 应用。

## PC 新增功能

| 功能 | 已支持的能力 |
|---|---|
| 显示与输入 | LVGL + SDL 窗口、鼠标模拟触摸和键盘输入、中文字体，复用 Scene/Raster/Direct Surface 服务。 |
| Home 面板 | 参考 MixPad 4 的深色界面，显示模拟时钟、天气、下一次闹钟、Note 事项与音乐播控；支持应用目录和市场，音乐后台继续播放。 |
| 官方应用市场 | 浏览、搜索和查看版本，下载校验并安装支持的官方 RV32 AOT Bundle，由 QEMU 执行；portable Wasm 直接由本机 WAMR 解释执行。 |
| 网易云音乐 Guest | 无 API 网关直连，支持官网微信扫码登录、个人资料与歌单、每日推荐、搜索、红星收藏、滚动列表与播放控制；完整播放或试听以服务端权限为准。 |
| 网络与音频 | POSIX socket、mbedTLS、Guest 异步 HTTP/HTTPS、Host MQTT/MQTTS 连通性测试，以及本地／网络 MP3、WAV、Ogg/Opus 文件的 SDL 立体声播放。 |
| 天气与后台任务 | Open-Meteo 直连，三座城市、温度/湿度/风速与成功缓存；持久 Job、有限工作票据、冷/热/挂起唤醒与用户关闭策略。 |
| 日历与提醒 | 月历、六条可编辑/完成/删除的 Note 提醒、单次/每周闹钟；Host 持久登记，关闭应用仍响铃，弹窗输入优先并压低音乐音量。 |
| Guest UI SDK | IconButton、ListItem、ScrollView、Slider，共享面板配色/可缩放数字、公历计算，以及下载图片的 PNG/JPEG 纹理解码。 |

PC Home 默认四个原生 Wasm 槽，天气、时钟与音乐共存；`--single-app` 保留单槽兼容路径。
Activity/Service、资源归属、Widget、消息、Job、闹钟与焦点使用同一套 SDK，见
[组件契约](docs/design/app-components.zh-CN.md)、[D4](docs/development/app-manager-d4.zh-CN.md)、
[D5](docs/development/app-manager-d5.zh-CN.md)、[D6](docs/development/app-manager-d6.zh-CN.md)和
[D7](docs/development/app-manager-d7.zh-CN.md)。RV32 多实例与混合后端已接入，见
[D8](docs/development/app-manager-d8.zh-CN.md)；MCU 多实例留在 D9。
新增 HTTP 与 URL 音频适配器目前用于 PC，MCU 需要补齐对应平台适配。
Home、音乐、天气和日历使用统一的深灰与橙色控件，详见
[面板界面设计](docs/design/panel-ui.zh-CN.md)和[运行与验证](docs/development/panel-ui.zh-CN.md)。

## 在 PC 上运行

使用带图形和音频支持的 Ubuntu 22.04 或 WSL2，准备 WASI SDK、CMake、Ninja、Python 3，
以及 SDL2、libpng、cJSON 开发包；依赖安装详见 [PC 运行指南](firmware/pc/README.zh-CN.md)。
在仓库根目录执行：

```sh
bash tools/pc.sh deps
bash tools/pc.sh configure
bash tools/pc.sh build
bash tools/pc.sh run --home
# RV32 多实例，自动构建 runner 和三个新 SDK AOT 包。
bash tools/pc.sh run --home --rv32

# 单独打开官方市场或音乐登录界面。
bash tools/pc.sh run --market
bash tools/pc.sh run --music-login
```

`--home` 会构建天气、时钟和音乐 Guest；音乐命令自动构建音乐 Guest。按 `Home` 或 `F1` 返回 Home，保留后台运行；
Guest 在前台时按 `Esc` 停止应用。
运行官方 RV32 AOT 还需要 `qemu-system-riscv32` 和支持 `rv32imafc/ilp32f` Newlib 的裸机 GCC，
先执行 `bash tools/pc.sh rv32-build`，再从市场安装。当前不支持 Xtensa AOT 或 Wi-Fi 硬件模拟，
音频文件下载完成后播放。

详见 [PC Home](docs/design/pc-home.zh-CN.md)、[RV32 运行方案](docs/design/pc-rv32.zh-CN.md)
和[网易云音乐应用指南](guest/apps/netease-music/README.zh-CN.md)。

## 支持硬件

| 芯片 | 开发板 |
|---|---|
| ESP32-P4 | [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4) |
| ESP32-P4 | [酷世DIY ESP32-P4C5 4.3 寸](https://github.com/kevincoooool/ESP32P4_KSDIY) |
| ESP32-S31 | [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) |
| ESP32-S3 | [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) |
| ESP32-S3 | [立创 SZPI](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html) |
| ESP32-S3 | [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) |
| ESP32-S3 | [SenseCAP Watcher](https://wiki.seeedstudio.com/cn/getting_started_with_watcher/) |

Host 使用 ESP-IDF 6.1 和固定版本的 [WAMR fork](https://github.com/78/wasm-micro-runtime)。
MCU 应用按目标架构编译为 AOT v6 Bundle，ABI 仍在演进。

## 开发应用

安装 [SDK](https://micropixel.ai/docs/environment/)，连接已运行 MicroPixel 固件的设备。
应用开发不需要 ESP-IDF。

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
micropixel --transport usb run
```

`run` 构建、安装、启动应用并跟随日志。Ctrl-C 退出日志跟随，应用继续运行。
详见[快速入门](guest/sdk/QUICKSTART.zh-CN.md)与[发布指南](guest/sdk/PUBLISHING.zh-CN.md)。

## 构建固件

初始化子模块，激活 ESP-IDF 6.1，配置 WASI SDK 和匹配的 MicroPixel WAMRC。
工具链安装与烧录步骤见[构建指南](docs/development/flashing.zh-CN.md)。

```sh
git submodule update --init --recursive
python3 -m pip install -r requirements-dev.txt
bash tools/p4.sh build-host
```

其他板型：`bash tools/s31.sh build-host`、`bash tools/s3.sh build-host <box3|szpi|cores3|watcher>`、
`python3 tools/firmware.py <ksdiy-p4c5|ksdiy-p4c5-landscape> build`。

## 项目文档

- [Guest SDK](guest/sdk/README.zh-CN.md)：应用接口与示例。
- [Firmware](firmware/README.zh-CN.md)：Host 运行时与板级支持。
- [文档索引](docs/README.zh-CN.md)：架构、协议与开发指南。
- [参与贡献](CONTRIBUTING.zh-CN.md)。

## 许可证

项目自有代码、文档和素材默认采用 [Apache-2.0](LICENSE)。
依赖许可证与例外见[第三方声明](THIRD_PARTY_NOTICES.md)。
