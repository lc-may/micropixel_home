# Linux PC Host

在 WSL2 Ubuntu 中运行原有单线程 Wasm 应用，复用固件的 Guest ABI、Service Registry、
Scene/Raster/Direct Surface 图形、输入事件、定时器、资源和 Opus/PCM 播放服务。
`main.cpp` 是 PC 的组合入口；ESP-IDF 目标继续使用原有 `FirmwareApp`。

## 构建和运行

在仓库根目录执行。基础依赖为 Git、Python 3、CMake、Ninja、SDL2、libpng 和 cJSON 开发文件。
Guest 构建通过 `WASI_SDK_PATH` 查找 WASI SDK；存在 `/opt/wasi-sdk` 时自动使用它。

```sh
# 下载固定版本源码，将 Ubuntu 22.04 的编译器和开发库解包到 build/，不需要 sudo。
bash tools/pc.sh deps
bash tools/pc.sh configure
bash tools/pc.sh build

# 无需 MCU 的 wamrc，直接构建原应用的 portable Wasm。
bash tools/pc.sh guest guest/apps/snake
bash tools/pc.sh guest guest/apps/sdk-demo
bash tools/pc.sh run build/pc-apps
bash tools/pc.sh run --autostart build/pc-apps/snake/snake.pc.json
```

PC 应用由 `.wasm`、原格式的 `generated/resources.pack` 和 `.pc.json` 描述文件组成。
Host 校验资源范围、重叠、格式、FNV 内容哈希和 SHA-256 目录摘要，再交给原资源服务。
RISC-V Bundle 可通过下面的 RV32 后端执行；Xtensa AOT 当前不支持。
portable Wasm 仍直接在 x86 WAMR 中解释执行，不需要 MCU 的 wamrc。
每个实例使用独立 AppSession；原生与 RV32 的线性内存上限均为 16 MiB。

鼠标左键映射触摸，方向键/WASD 控制方向，Enter/空格确认，Backspace 返回，Z/X 映射游戏按键。
`Home` / `F1` 返回系统 Home，Guest 在后台继续运行，音乐和自动下一首不中断。
Home 的音乐卡片、应用目录中的“返回应用”或再次按 `Home/F1` 复用原会话，保留登录和播放队列。
Home 显示数字时钟、天气、闹钟与事项，点击时间/天气主卡或四块功能卡片导航；“应用”打开目录、市场、音量和关闭操作。
音乐卡片支持暂停/继续、下一首，系统主音量位于应用目录底部；`Esc` 从 Guest 返回 Home，保留后台播放。
应用目录的“关闭当前应用”停止最近的当前应用；在 Home 再按 `Esc` 保持主页，在市场或闹钟弹窗中按 `Esc` 仍执行返回或停止响铃。
默认分辨率 720×720，可用 `--width`、`--height` 调整；未完整移植固件的系统菜单、状态层和硬件转场。
软件合成采用单个 App Surface，与 LVGL 刷新共用适配器锁，避免并发绘制导致保留文字丢失。

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home
bash tools/tests/test_firmware_host.sh --pc --launcher
# 使用已构建的音乐 RV32 release Bundle
bash tools/tests/test_firmware_host.sh --pc --launcher --rv32
```

`--home` 会增量构建天气、时钟、音乐和新闻 Guest，再打开四槽 Home；已安装 Bundle 也会列出。
Home 是 `firmware/pc/home_app.*` 中的 Host 系统应用，前台 Activate 由它调度；授权逻辑消息可以冷启动目标 Service。
天气、时钟、音乐和新闻可以共存，空闲 Job Service 挂起，媒体 Service 保持有效播放。`--single-app` 使用单槽兼容模式，切换应用会停止旧 Guest。
生命周期与呈现边界见 [PC Home](../../docs/design/pc-home.zh-CN.md)。

Home、天气与闹钟按 [720×720 设计稿](../../docs/ui_design/index.html)使用数字主卡、四块功能卡片和 Material 圆润图标；音乐仍直连网易云。
日历支持月份切换，事项支持键盘/输入法、提醒时间、完成/恢复和删除，最多六条。
未来事项由 Host 保存，到期时即使 Clock Guest 已停止仍弹窗响铃；重启后保留已确认的计划。
天气显示温度、湿度、风速和更新时间，缺少的数据用 `--`。
旧数据中曾手动关闭天气时，点击“天气”重新启动刷新。
应用目录中的[新闻热榜](../../guest/apps/news/README.zh-CN.md)直连今日头条，提供标题、热度、排名、已读过滤与缓存；浏览新闻时音乐继续播放。
Home 音乐播控采用 168×60 的圆角矩形背景，64×48 的可见主按钮和独立的 84×84 触摸区域。
指定 `--screenshot build/panel.png` 后按 F12 保存当前画面。
详见[界面设计](../../docs/design/panel-ui.zh-CN.md)和[自动验证](../../docs/development/panel-ui.zh-CN.md)。

```sh
bash tools/tests/test_firmware_host.sh --pc --panel
bash tools/tests/test_firmware_host.sh --pc --panel --rv32
```

## 官方应用市场

`bash tools/pc.sh run build/pc-apps` 的大厅右上角有「应用市场」按钮。也可直接打开市场或指定应用详情：

```sh
bash tools/pc.sh run --market
bash tools/pc.sh run --market-app local.tommy.liquidduck
```

市场通过[官方公开 HTTPS API](https://micropixel.ai/docs/specs/control-api-v1.openapi.json)加载实时列表、
封面和详情，无需登录或设备配对。支持按名称、简介或 App ID 搜索，每页显示四项；
点击卡片查看介绍、版本、包大小及目标架构。刷新可重新请求，Esc 返回列表，再按 Esc 返回本地大厅。
网络请求和封面解码使用可取消的工作线程，退出页面不会阻塞 LVGL。

构建 RV32 后端后，详情页对可用的 `riscv32-ilp32f` 版本提供「安装并运行（RV32）」按钮。
下载先验证市场记录中的大小、SHA-256、Bundle 内容、App ID 和版本，再原子替换本地安装文件。
此流程使用公开 HTTPS 下载和 TLS 信任，不使用设备远程安装协议或设备侧 JWS 凭据。
本地 portable Wasm 应用继续从大厅启动；仅有 Xtensa 或字体组件的条目不提供应用安装。
列表上限 128 项、每项最多 32 个未撤回版本、单次响应/封面上限 2 MiB；封面支持 PNG/JPEG。
`MICROPIXEL_PC_MARKET_URL` 可覆盖默认服务地址，用于本地联调。

中文界面默认读取系统 Noto Sans CJK、文泉驿或宋体 TrueType 字体。缺少字体时安装 `fonts-noto-cjk`，
或用 `MICROPIXEL_PC_FONT=/path/to/font.ttf` 指定本地 TTF/TTC（不超过 32 MiB）。
字体由现有 LVGL Tiny TTF 渲染，不下载或加载市场发布的字体组件。
Home 的大号数字使用 `fonts-noto-core` 中的 Noto Sans SemiBold；Guest 的数字子集与 Material 图标已随 SDK 提供。

## RV32 官方 AOT 运行

需要 `qemu-system-riscv32` 和带 Newlib、libm、`rv32imafc/ilp32f` multilib 的裸机 GCC。
构建脚本依次查找 `MICROPIXEL_RV32_CC`、PATH 中的 RISC-V 裸机 GCC，以及已有 Bouffalo SDK
的工具链。无需安装 RV32 Linux、交叉编译 SDL，或下载应用源码。

```sh
# 必要时指定编译器；本机已有合适工具链时可省略。
export MICROPIXEL_RV32_CC=/path/to/bin/riscv64-unknown-elf-gcc
bash tools/pc.sh rv32-build
bash tools/pc.sh build

# 官方市场中点击安装，或按 App ID 下载、安装并立即运行。
bash tools/pc.sh run --market
bash tools/pc.sh run --install-app com.lckfb.jump

# 直接运行已下载的官方 RISC-V Bundle。
bash tools/pc.sh run --autostart path/to/app.bundle.bin
```

安装位置为 `build/pc-data/apps/<app-id>.bundle.bin`，随 `MICROPIXEL_PC_DATA_DIR` 覆盖。
不带应用参数启动时，大厅同时发现本地 Wasm 与已安装 Bundle。Esc 返回大厅，可再次启动。
`MICROPIXEL_QEMU_RV32` 和 `MICROPIXEL_RV32_RUNNER` 可覆盖模拟器及运行程序路径。

后端在 QEMU `virt` 单核 RV32 M-mode 中执行同一固定 WAMR fork 的 AOT v6；
通过实例私有共享 RAM 和 Unix socket 代理七个 Core imports，复用现有 Host 服务。
UART 使用中断唤醒，等待事件时运行程序休眠。退出先请求 Guest 停止，再限时回收 QEMU。
协议不改变 Guest ABI，Wasm 和 AOT 不互相转换。[设计边界](../../docs/design/pc-rv32.zh-CN.md)。
连续 15 秒未收到运行程序消息会结束会话；Guest 通过 `event_wait` 等待事件不受此限制。

当前只支持有内存检查的单线程 RV32 AOT v6，Bundle/AOT 上限 8 MiB、线性内存上限 16 MiB。
Guest-owned Direct Surface 缓冲区暂不开放；Host-owned 缓冲区可用。Wi-Fi、真实传感器、GPIO
等硬件仍不可用。Network 1.1 HTTP 与 Audio 1.1 URL media 已有 PC 适配器，可经相同 RV32 桥调用。
模拟运行的帧率和兼容性取决于应用，不能据此推断 MCU 真机性能。

## 音频

采用 SDL2 Audio；在 WSLg 中由 PulseAudio 输出到 Windows 音频设备。
已有 `/mnt/wslg/PulseServer` 时无需安装另一个 Windows 音频服务器。必要时显式指定：

```sh
PULSE_SERVER=unix:/mnt/wslg/PulseServer SDL_AUDIODRIVER=pulseaudio \
    bash tools/pc.sh run build/pc-apps
```

支持原 Guest 的合成音效、Ogg/Opus 资源和 PCM 流。大厅输入框可播放本地 MP3、PCM WAV、Ogg/Opus，
以及 HTTP/HTTPS 文件 URL。下载和解码在可回收的工作线程执行，SDL 回调只读取 PCM 并混音。
输出统一为 48 kHz、双声道 signed 16-bit，沿用 Host 主音量曲线。

```sh
bash tools/pc.sh run --play ./voice.wav
bash tools/pc.sh run --play https://example.com/voice.ogg
```

网络文件先下载，再解码播放。压缩文件上限 32 MiB、解码 PCM 上限 128 MiB。
Host 媒体保留立体声，输出为 48 kHz S16；MP3 使用 mpg123，WAV 使用 SDL，Opus 使用 opusfile。
切歌和停止通过单项替换队列取消旧请求，不在 UI 线程等待下载完成；支持暂停和内存中跳转。
当前不支持 AAC/FLAC、无限时长流媒体或边下载边播放。Guest 的 mono PCM 契约保持不变。

## 网易云音乐终端

网易云是 [SDK Guest 应用](../../guest/apps/netease-music/README.zh-CN.md)，使用 Scene、
TextButton、单线程事件循环、Network HTTP 与 Audio URL 服务。`run --music` 自动增量构建 Wasm。

```sh
bash tools/pc.sh build
bash tools/pc.sh run --music
bash tools/pc.sh run --music-login
bash tools/pc.sh run --music-query 纯音乐
bash tools/pc.sh run --music-song 139774
```

普通大厅右上角的「网易云」按钮也可启动它；首次使用先运行上述 `--music` 命令构建应用。
支持键盘搜索、个人歌单、滚动推荐、红星收藏、上一首/下一首、暂停/恢复、停止和拖动进度。
Esc 返回大厅，系统音量由 Host 管理。
右上角“登录”使用网易云官网的微信扫码授权；支持昵称、退出和可选记住登录，详见应用文档。

Guest 内实现网易云 weapi 请求编码和 JSON 解析，直接连接 `music.163.com` 与官方 CDN。
不需要 Node.js/API 网关；匿名状态能否完整播放或试听，以服务端返回的权限为准。
Host 提供通用 HTTPS、MP3/WAV/Opus 解码与 SDL 立体声输出。当前下载完成后播放，尚未实现流式边下边播。

接口、容量与资源回收约束见 [设计](../../docs/design/pc-music.zh-CN.md)。当前适配器仅覆盖 PC；
原有 MCU 固件尚无这些新服务。旧的原生网易云 UI/诊断入口已由 Guest 实现替代。

## 网络

Linux 内核提供 TCP/IP 和 DNS，使用 POSIX socket，不引入 lwIP，不模拟 Wi-Fi 扫描或连接。
coreHTTP 3.1.1 提供 HTTP/1.1，coreMQTT 2.3.1 提供 MQTT 3.1.1，共用 socket/mbedTLS 传输。
TLS 默认验证系统 CA、服务端名称和证书有效期；私有 CA 通过 `MICROPIXEL_CA_FILE` 指定。

```sh
bash tools/pc.sh run --http https://example.com/ --output build/example.html
bash tools/pc.sh run --mqtt mqtt://127.0.0.1:1883 --topic micropixel/pc/test --message hello
bash tools/pc.sh run --mqtt mqtts://broker.example.com:8883 --topic micropixel/pc/test --message hello
```

MQTT 命令连接 Broker、订阅主题、发布消息并等待自身消息返回，验证双向连通性。
当前不提供认证参数、持久订阅管理或完整 MQTT 应用 API。
TCP 连接、TLS 握手和 HTTP 发送/接收空闲等待均设置为 10 秒；DNS 使用系统 resolver，其超时由系统配置决定。

Network Service `19` 现已提供有界异步 HTTP，Guest 使用 `app.network()`，不直接调用 Linux socket。
MQTT 当前仍是 Host 连通性测试接口，尚未公开为 Guest SDK 能力。
原设备 Remote Control、远程安装和 OTA 的 HTTP/3/QUIC 控制通道尚未移植到 PC；
上面的市场浏览使用独立的公开 HTTPS REST 接口。
Wi-Fi、蜂窝、I2S、真实传感器和 GPIO 不在 PC 目标中注册。

## 数据和验证

KV 使用 SQLite 持久化，默认路径为运行目录的 `build/pc-data/storage.sqlite3`。
`MICROPIXEL_PC_DATA_DIR` 可为不同实例指定独立目录。

```sh
bash tools/tests/test_firmware_host.sh --pc
bash tools/tests/test_firmware_host.sh --pc --market
# 可选：在独立 X11/WSLg 窗口验证点击、翻页、刷新和 Esc，不移动用户鼠标。
bash tools/tests/test_firmware_host.sh --pc --market --interactive
# RV32 回归首次可下载固定版本的官方 Jump Bundle，后续使用 build/ 中的校验缓存。
bash tools/tests/test_firmware_host.sh --pc --rv32
bash tools/tests/test_firmware_host.sh --pc --rv32 --interactive
# 音乐回归使用 ffmpeg 生成合成 MP3；实际播放器不依赖 ffmpeg 或 Node.js。
bash tools/tests/test_firmware_host.sh --pc --music
bash tools/tests/test_firmware_host.sh --pc --music --interactive
bash tools/tests/test_firmware_host.sh --pc --login
bash tools/tests/test_firmware_host.sh --pc --login --interactive
bash tools/tests/test_firmware_host.sh --pc --run --headless --autostart \
    --seconds 3 --screenshot build/snake-pc.png build/pc-apps/snake/snake.pc.json
```

回归运行原 ABI、合成音、PCM 背压、定时器、Scene 和 Direct Surface 应用，
并通过本地 HTTP/TLS/MQTT 服务验证网络文件播放、分块传输、重定向和证书拒绝路径。
市场回归覆盖列表/详情与封面实际渲染、撤回版本过滤、空列表、坏 JSON、超限和 HTTP 错误。
RV32 回归执行官方 AOT，验证画面、坏包/架构拒绝、IPC 指针和帧边界、安装失败保留旧包；
X11 交互回归验证触摸、非静音 PCM、重复启动/退出和市场安装按钮。
音乐回归使用 loopback 服务验证匿名 POST 请求、完整/试听/拒绝状态、跳转和非网易云音源拒绝、
MP3 双声道、暂停/跳转、重播、导航及下载取消。测试不依赖真实音乐账号或第三方网关。
SDL disk 驱动验证实际输出 PCM 非静音；WSLg 窗口和扬声器需要交互和试听确认。
`--headless` 使用 SDL dummy 显示；已有 `SDL_AUDIODRIVER` 设置会保留，以便捕获音频。

`compat/` 是仅用于 PC 的运行适配层，提供真实线程、互斥量、固定容量队列、单调时钟和定时器。
Linux 决定线程 CPU 和优先级；不模拟 FreeRTOS 的实时调度。该目录不参与 ESP-IDF 固件构建。


## D1–D3：多实例与组件开发配置

Home 默认四槽，`--single-app` 保留旧应用兼容路径；`--multi-app` 显式启用开发配置。
新组件使用 requirements v2 + Lifecycle 1.0，允许 Activity 销毁后 Service 保持运行。
旧 v1 应用在多槽配置下隐藏即挂起；音乐已使用独立 PlaybackService。

```sh
bash tools/pc.sh guest guest/apps/components-demo
bash tools/pc.sh run --multi-app --autostart build/pc-apps/components-demo/components-demo.pc.json
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --multi-app
# 本地 HTTP + X11/WSLg 交互自动验证
bash tools/tests/test_firmware_host.sh --pc --components
```

详见[组件与资源契约](../../docs/design/app-components.zh-CN.md)及[D7](../../docs/development/app-manager-d7.zh-CN.md)。Widget、消息、Job、天气和 Host 闹钟已接入；RV32 多实例与混合后端见 [D8](../../docs/development/app-manager-d8.zh-CN.md)，MCU 适配留在 D9。

## RV32 多应用

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home --rv32
bash tools/tests/test_firmware_host.sh --pc --rv32-multi
bash tools/tests/test_firmware_host.sh --pc --rv32-suite
```

`--rv32` 为默认内置应用选择 RV32 release Bundle；显式传入的 `.pc.json` 或 Bundle 按自身后端运行，
因此可混合原生音乐与 RV32 天气，或反向组合。每个 RV32 槽独立 QEMU、RAM 和 socket。
四槽、576 MiB 准入预算和最多四个子进程共同限制并发数。
运行条件、产物与能力矩阵见 [D8](../../docs/development/app-manager-d8.zh-CN.md)。
