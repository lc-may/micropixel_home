# BL618DG Orion 外设接入

## PC Host 移植

`HOST_PORT=1` 在 Orion 的 BL618DG B1 芯片上使用 AP 核运行 FreeRTOS Host。
板级构建选择 `CPU_MODEL=b1`。完整 Host 使用 SDK 预编译无线库和 MP3 解码库的原生 ILP32D ABI。
应用和 conformance Guest 通过与 PC 相同的 `tools/pc.py` 构建，默认用固定版本的 `wamrc`
编译为 RISC-V ILP32D AOT v6。
WAMR 沿用 Core ABI、`ServiceRegistry`、`EventQueue` 和 `AppManager`，固件提供四个 Guest 槽。
Guest 线性内存上限为 4 MiB，支持时钟、日志、事件等待、硬件随机数、语言和启动参数，
以及定时器、合成音效和 PCM 流。定时器支持一次触发、周期、取消和销毁，Guest 在
`event_wait` 取走到期事件，周期跨过多次截止时间时合并为一次事件。
构建时将应用压缩到 `orion_apps.bin`。Host 用固件内的 SHA-256 校验 `apps` 分区，再将
各应用在 PSRAM 中解压、重定位并执行。AOT 应用停止采用协作事件。`ORION_AOT=0` 构建带指令预算的
解释器固件，应用字节码随固件嵌入。

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion HOST_PORT=1 \
    BL_SDK_BASE=/path/to/clean/bouffalo_sdk
```

将 WASI SDK 和 [Bouffalo 构建工具链](../README.md)准备好后，从仓库根目录构建。
把固定版本的 WAMR `wamrc 2.4.3` 加入 `PATH`，或通过环境变量 `WAMRC` 指定。
Orion Host 的云端文字字库在构建时使用 Pillow 和 fontTools 生成，构建会下载并校验固定版本的思源黑体。
离线构建可通过 `ORION_FONT_SOURCE=/path/to/SourceHanSansSC-Regular.otf` 指定同一版本的本地文件。
默认 `RADIO=wifi`，构建产物在 `build-orion-host-wifi/build_out/micropixel_orion_host_bl618dg_ap.bin`。
烧录前对副本执行 SDK 后处理，将 Orion BSP 继承的 RF 参数表加入应用镜像。
`BL_SDK_BASE` 指向构建使用的 SDK 绝对路径。

```sh
cp firmware/bouffalo/boards/bl618dg/build-orion-host-wifi/build_out/micropixel_orion_host_bl618dg_ap.bin \
    /tmp/micropixel_orion_host.bin
"$BL_SDK_BASE/tools/bflb_tools/bflb_fw_post_proc/bflb_fw_post_proc-ubuntu" \
    --chipname=bl618dg --imgfile=/tmp/micropixel_orion_host.bin --appkeys=shared \
    --ptfile="$PWD/firmware/bouffalo/boards/bl618dg/orion/config/partition_cfg_8M.toml" \
    --dtsfile="$BL_SDK_BASE/bsp/board/bl618dgdk/config/bl_factory_params_IoTKitA_auto.dts"
```

按 [SDK 烧录流程](runtime.md#validation-executable) 检查设备、分区表与容量。
默认 8 MiB 布局将同一构建目录下的 `orion_apps.bin` 写入 `apps`（`0x700000`），并将
处理后的固件写入选中的 `FW` 槽。通过串口的 `MPX AOT catalog verified` 和
`MPX LOAD ... backend=aot` 确认板端运行。AOT 应用包和固件通过摘要绑定，需要配套烧录。
启动后，在 LCD 首页点天气、闹钟、网易云音乐或新闻，也可从「全部应用」打开。
应用和子页面的底部常驻「返回上一页」按钮。每次点击返回一层，在应用入口页点击会回到 Home。
从底部中央上滑可直接回到 Home。
Orion 只向前台 Session 发送触控并显示其画面。切换应用时，已有 Session 保留在各自的槽中。
每个 Session 持有自己的事件队列、图形资源和网络请求句柄。槽位用尽时可以用 `mpx stop <app>` 释放一个槽。
也可通过 2 Mbps 板载 UART 使用以下命令。

```text
mpx list
mpx status
mpx perf
mpx home
mpx back
mpx run settings
mpx run weather
mpx run news
mpx run netease-music
mpx run netease-music --song=139774
mpx run netease-music --login
mpx run clock-alarm
mpx stop clock-alarm
mpx run timer_counter
mpx run sdk_hello
mpx run main_failure
mpx run audio_pcm_stream
mpx stop audio_pcm_stream
```

`sdk_hello` 验证 SDK 启动及基础服务，预期退出码为 `0`。`main_failure` 预期退出码为 `7`。
`audio_pcm_stream` 播放 1 kHz 正弦波并检查 PCM 流，完成后报告非零 DMA 写入次数。
`timer_counter` 验证一次触发、周期、取消、合并及句柄回收。
`mpx list` 查看当前状态和上次退出码，`mpx status` 读取 LCD 帧数与非零 PCM 写入计数。
停止请求通过 Guest 事件队列送达。

闹钟使用原版 Guest 的 Activity 和 Service 入口。可用串口命令 `mpx time <UTC 秒数>`
设置本次开机的墙钟，再用 `mpx run clock-alarm --alarm-test` 运行 Guest 的五秒响铃测试。
主闹钟与各事项的提醒时间、重复规则、启用状态和上次触发记录保存在 `mp_kv` 的 `clock.alarms.v1` 文件中。
文件同时保存时区和时钟设置，开机恢复记录，校时后重新计算下次触发。事项完成状态保存在 `clock.notes`。

`boards/bl618dg/orion/` 定义 LCD、触控、音频引脚和 Host 组合入口。
`runtime/` 提供 FreeRTOS Guest Runner、标准 Core imports 及服务端点，复用共享的
`AppManager`、`ServiceRegistry`、`EventQueue` 和 PCM 流实现。

| PC 功能 | Orion 接入 |
| --- | --- |
| 原生 Wasm、应用目录与生命周期 | 触控和串口操作内置目录，复用 `AppManager` 管理四个 Session。首页可启动配置、闹钟、天气、网易云音乐和新闻。媒体 Widget 和点对点播放控制复用 `AppHub`。 |
| SDL 显示和触控、Home 与应用 UI | Orion 和 PC 共享 Home 的布局、配色和 Material 图标，480×480 LVGL 对接 ST7102 与 ST7123。前台 Guest 支持逻辑画布缩放、文字、裁剪、动态纹理和 JPEG/PNG 解码。天气和音乐 Widget 接收各自后台 Session 的最新快照。 |
| 文件和 SQLite、资源包、本地安装 | Wi-Fi 配置和天气缓存分别保存为 `mp_kv` 上的 LittleFS 文件。Guest 通用 KV、BundleFS 和资源安装待接入。 |
| POSIX 网络、HTTPS/MQTT、市场及联网 Guest | 配置 Guest 管理 Wi-Fi STA。Network 1.3 提供活动和后台 Service 两条 HTTPS 通道，支持 GET、POST、响应头和接收超时。明文 HTTP、MQTT、市场待接入。 |
| 音效、PCM、压缩音频和 URL 媒体 | 音效与 PCM 输出和 LCD 可共用同一固件。网易云音乐的 HTTPS MP3 走 Host 混音器，支持暂停、进度跳转和后台播放。录音及其他 URL 音频格式待接入。 |
| RV32 AOT | Orion 生产目录使用 SDK 的 ILP32D。公共 ILP32F Bundle 在独立验证固件运行。 |

## 内存与启动性能

Orion 默认开启 `CONFIG_PSRAM_COPY_CODE`。SDK 启动阶段从 Flash 复制常规 Host 指令和只读数据，
完成搬运后才启动 Host。启动、时钟切换和 Flash 操作所需的专用代码段按芯片要求链接。
Guest 镜像在 PSRAM 中解压、装载和执行，模块、实例与线性内存也分配在 PSRAM。

OCRAM 存放 Host、Guest、音频等热路径的任务栈、控制结构及片内驱动数据。HTTPS 和媒体任务
优先用 WRAM 的空闲堆，并为无线保留 64 KiB。帧缓冲、字库、场景和应用数据分配在 PSRAM。
后台 Session 首次可见且提交场景时才分配像素缓冲。XZ 解码在 OCRAM 有余量时使用片内工作区，
否则回退到 PSRAM。

| Orion 栈 | 容量 |
| --- | ---: |
| Host 控制和合成 | 12 KiB |
| 各 Guest 对应的 FreeRTOS 任务 | 12 KiB |
| 每个 HTTPS 通道、媒体任务 | 16 KiB |
| 音频混音任务 | 8 KiB |
| FreeRTOS Idle、Timer | 1 KiB、6 KiB |
| WAMR 执行栈、Guest 线性内存中的 C/C++ 栈 | 16 KiB、32 KiB |

`mpx perf` 输出代码段容量、各内存区的剩余空间和最大连续空闲块，以及每个 FreeRTOS 任务的
栈水位和所属内存区。`MPX LOAD` 给出 Guest 解压或复制、WAMR 装载和实例化的微秒数，
`Orion Guest frame displayed` 给出从分配应用画布到首帧合成的微秒数。

比较编译选项时使用独立的 `BUILD_DIR`。可通过 `ORION_RUNTIME_OPT=-Os|-O2|-O3` 选择
部分 Host 和软件图形合成的优化级别。`ORION_AOT=0` 下可用 `ORION_INTERPRETER_OPT` 单独设置解释器编译级别。
通过 `ORION_GUEST_PROFILE=release|performance`
选择内置 Guest 的优化级别。Orion 的 FW 槽为 3 MiB，烧录前检查 SDK 后处理后的完整镜像容量。

## 8 MiB Flash 分区

Orion Host 指定 [partition_cfg_8M.toml](../boards/bl618dg/orion/config/partition_cfg_8M.toml)。
它适用于 JEDEC 容量为 8 MiB 的设备。原 `mfg` 和 `media` 区间用于扩展主固件槽和存放图片，
Boot2、系统配置与工厂数据沿用原地址。

| 区域 | 起始地址 | 容量 |
| --- | --- | --- |
| Boot2 和双份分区表 | `0x000000` | 64 KiB |
| FW 主槽 | `0x010000` | 3 MiB |
| imgres | `0x310000` | 868 KiB |
| PSM、KEY、DATA、factory | `0x3e9000` | 92 KiB |
| FW 备用槽 | `0x400000` | 3 MiB |
| apps | `0x700000` | 896 KiB |
| mp_kv（LittleFS） | `0x7e0000` | 128 KiB |

从旧版迁移时，先备份要重新分配的区间与双份分区表。写入并读回校验固件槽后，再切换分区表。
默认固件从 `apps` 装载已校验的 AOT 目录，`imgres` 保留给图片。
两个 FW 槽共用 `apps` 分区。切换到摘要不同的 AOT 固件时，需要写入它配套的应用包。

## Wi-Fi 回连与天气刷新

Orion 从 `mp_kv` 挂载 LittleFS。Wi-Fi 保存最近一次成功连接的 SSID、BSSID 和密码到 `settings.wifi`。
DHCP 模式在每次连接后重新取得地址。静态模式额外保存 IPv4、掩码、网关和 DNS，并在回连时恢复。
配置应用的连接入口使用 DHCP。现有 SDK 串口命令 `set_ipv4 <ip> <dns> <gateway> <netmask>` 可为已连接的 STA 设置静态值。
写入先关闭临时文件，再重命名提交。已有 Timebox 配置可在首次成功连接后迁移到文件。

拿到 IP 后，Host 通过 SNTP 校时，成功后每 15 分钟重校。每次校时成功都会更新 TLS、Home 和闹钟的 UTC 基准。
网络中断期间，Home 和闹钟继续使用最近一次 UTC 基准与单调时钟走时。串口手动设置的 UTC 基准在下一次成功校时时更新。
天气应用通过 Network Service 请求 Open-Meteo，应用启动、切换城市及点击刷新时发起更新。
天气 Service 在开机后占用独立 Session。首次启动先从 LittleFS 的 `weather.v1` 加载最近一次天气，
再刷新并发布给 Home。退出天气页面时刷新一次，随后每 15 分钟更新，网络暂不可用时会重试。
天气 Session 在其他应用显示时继续运行。

## 网易云音乐

Orion 运行与 PC 相同的网易云 Guest。搜索、账号和歌曲 URL 解析继续使用 Guest 的网页协议。
Network 1.3 中的默认接收超时是 10 秒，Guest 可为微信长轮询单独指定超时。每条请求的响应正文最多
256 KiB，响应头最多 8 KiB。HTTPS 使用固件的 CA 证书包校验服务器。交换接口把 HTTP 状态和重复的
响应头交给 Guest，Cookie 跟随 Guest 的域名与路径规则。

HTTPS 每次最多读取 4 KiB 明文，读取缓冲在 PSRAM。多个 TCP 连接共用 WRAM 中的 64 个 Wi-Fi RX
缓冲，lwIP 通过自定义 `pbuf` 引用这些包，释放 `pbuf` 时归还缓冲。lwIP 堆为 96 KiB，
活动 TCP PCB 池容量为 15，netconn 池容量为 24，TCP 段池容量为 96。每条连接的接收窗口为
23,360 字节，乱序队列最多保留 3 个 `pbuf`。

Wi-Fi 的 MAC/FHOST 驱动使用 SDK 预编译库。RX 缓冲池由 SDK 的
`fhost_config_buffer.c` 重新编译，驱动启动时读取同一文件导出的数量和地址。
`ORION_WIFI_RX_BUFS` 选择 WRAM 池容量。默认 64，72 是已验证过的对照配置。
增加 8 个槽需要 13,440 字节的包缓冲。Orion 默认启用 lwIP SACK 输出，在握手允许后向发送端
报告已收到的乱序区间。`ORION_TCP_SACK=0` 可关闭此选项用于对照。[RFC 2018](https://www.rfc-editor.org/rfc/rfc2018.html)

在独立的 `BUILD_DIR` 使用 `ORION_RX_BENCHMARK=1` 构建并烧录诊断固件。
在同一局域网的 Linux 或 WSL 中，从仓库根目录运行 [LAN RX 压测脚本](../../../tools/test_orion_lan_rx.py)。
脚本会重启板子、暂停天气 Session 并向板端发送 15 路 TCP 数据。常用参数为
`mixed small full small:100 mixed:150 mixed:200 mixed:300`，冒号后的数值是每路每秒目标段数。
`--low-watermark 8` 对照 RX 空闲降到 8 个时每路保留一个乱序 `pbuf` 的策略。
`--strict` 要求所有连接在 12 秒内返回匹配的字节数，超过 12 秒的连接继续采样到 30 秒。

```sh
python3 tools/test_orion_lan_rx.py "$ORION_PORT" mixed small full mixed:200
```

应用支持触屏拉丁字符输入、JPEG/PNG 封面和二维码。开启「记住登录」后凭证保存在本机 LittleFS 的
`ncm.auth`，默认关闭。退出音乐页后 PlaybackService 继续运行，`mpx stop netease-music` 会结束该 Session。
Home 音乐卡片随 PlaybackService 更新当前曲目及播放状态，可直接暂停、继续播放或切换下一首。
板端边接收 HTTPS MP3 边用 SDK 解码，经 Host 混音器输出到 JY6316。解析器使用 2 KiB 输入窗口，
PCM 环形缓冲容纳 8,192 个单声道采样。开始播放和网络欠载后先积累约 200 ms PCM。暂停时保留已缓冲的数据，
缓冲满后暂停读取 HTTPS。进度跳转会重新请求音频，从帧头定位到目标，并预解码前面的帧以恢复 MP3 解码状态。
串口的 `--song=ID` 和 `--login` 参数在启动新 Session 时生效。

## 屏幕

Home 使用 13 px 正文与 21 px 日期字形。内置 Guest 优先使用 18 px、2 bpp 思源黑体界面字形。
网易云音乐页面和 Home 曲名共用 17 px 后备字库，收录思源黑体 SC 2.004R 中全部已映射汉字、
基础及扩展拉丁字母、常用标点和全角符号。固件内嵌压缩位图，开机一次解压到 PSRAM。
Home 的时钟和温度使用与 PC 同源的 Noto Sans SemiBold 数字子集。
新增中文文案后，使用 Adobe Source Han Sans SC Regular 2.004R 原始 OTF 和 PC 数字字体重新生成。

```sh
python3 tools/generate_orion_font.py /path/to/SourceHanSansSC-Regular.otf \
    --numerals /path/to/NotoSans-SemiBold.ttf
```

可在 Linux 或 WSL 通过 UART 保存当前 LCD 扫描帧。抓图期间 Host 暂停刷新，
工具检查完整帧的 CRC32 后将 480×480 RGB565 转为 PNG。

```sh
python3 tools/capture_orion_frame.py --port /dev/ttyACM1 build/orion-home.png
```

默认使用 Timebox 的 YDP395BT008-V1 配置。面板为 ST7102，480×480，双 lane MIPI DSI。
GPIO17 为低有效 LCD RESET，GPIO14 为高有效背光。SDK 从 PSRAM 中双缓冲刷出 RGB565。
`LCD_CONFIG=/path/to/lcd_conf_user.h` 可覆盖 480×480 DSI 面板配置，构建目录名追加 `-lcd`。

ST7123 的 I²C 地址为 `0x55`，RESET 为 GPIO18，INT 为 GPIO15，SDA 和 SCL 分别为 GPIO20、GPIO21。
面板初始化后按 Timebox 时序再释放触控复位。Orion Host 用实板上已验证的硬件 I²C0
读取触控报告，与 `xcodec_dev` 共用 Orion BSP 的总线互斥锁。LVGL 接收单指坐标。

LCD_RST 接 GPIO17，I²S MCLK 接 GPIO33（BOOT）。音频沿用 SDK Orion 的 Codec 和 I²S 初始化，
并按 Timebox 选择 BCLK 为 JY6316 时钟源，GPIO33 无需切换为 CLKOUT。
[Orion BSP](https://github.com/bouffalolab/bouffalo_sdk/tree/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/bsp/board/bl618dg_orion)

### 单独验证显示驱动

在 [Bouffalo 构建环境](../README.md)下，将已核对的面板、总线及引脚选项写入
SDK Make 格式的 defconfig 片段，再从 MicroPixel 仓库根目录构建。

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion \
    DISPLAY_CONFIG=/path/to/panel.defconfig
```

面板片段随硬件选择 `CONFIG_DISPLAY_DEFAULT_TYPE_GRAM` 或
`CONFIG_DISPLAY_DEFAULT_TYPE_VIDEO`，以及对应的面板、像素格式和板级选项。
MicroPixel 启用 SDK Display 默认端点与 LVGL 9，并在
`firmware/bouffalo/boards/bl618dg/build-orion-display/` 生成验证固件。
GRAM 构建同时运行 FreeRTOS/WAMR 验证。VIDEO 构建使用 SDK 的 ILP32D
DSI/DPU 驱动，生成独立的显示验证固件。VIDEO 与 RV32F AOT 的集成依赖
兼容的驱动 ABI 或单独的运行时移植。
更换面板时指定新的 `BUILD_DIR`，避免 SDK 复用已有的 menuconfig 结果。
SDK 的 [LVGL 示例](https://github.com/bouffalolab/bouffalo_sdk/tree/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/examples/display/lvgl_v9)
提供 ILI9341 SPI、ST7796 DBI 和 EK79007 DSI 的参考片段。

SPI、DBI、QSPI 和并行 RGB 还需通过 `DISPLAY_GPIO_SOURCE=/path/to/panel_gpio.c`
提供与接线匹配的 pin-mux 函数，函数声明见 SDK 的
[`display_default_gpio_ext.h`](https://github.com/bouffalolab/bouffalo_sdk/blob/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/bsp/common/display/default/display_default_gpio_ext.h)。
例如，SPI 实现 `board_display_spi_gpio_init/deinit`。Orion 替换了 DK 的
`board_gpio.c`，而 DK 的 SPI 默认时钟和数据引脚 GPIO17/19 在 Orion Host 上分别用于 LCD 复位和功放使能。 [Orion 板级引脚](https://github.com/bouffalolab/bouffalo_sdk/blob/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/bsp/board/bl618dg_orion/board_config.h)

显示任务独占 LVGL 实例，沿用 SDK 对 GRAM 异步绘制及视频帧所有权的处理。
出厂接线确认后，通过红、绿、蓝、白色条和左下角文字检查色序、方向、裁切和背光。
自定义面板应核对触控控制器和 I²C 接线，并使用 Orion BSP 的共享总线保护。

## 音频

### 运行 MicroPixel 音频 Guest

在 [Bouffalo 构建环境](../README.md)中设置 `WASI_SDK_PATH`，并将匹配 WAMR
版本的 `wamrc` 和两套 RISC-V 工具链加入 PATH。然后从仓库根目录构建。

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion AUDIO_TEST=1
```

SDK 根目录存放原理图等本地文件时，可通过 `BL_SDK_BASE` 指向同一提交的
干净 Git worktree 进行构建。

构建调用 MicroPixel SDK 编译 [audio_pcm_stream Guest](../../../guest/tests/conformance/audio_pcm_stream.cpp)，
把 RV32F AOT 嵌入 `build-orion-audio/build_out/micropixel_orion_audio_bl618dg_ap.bin`。
固件复用 Host 的混音器和 PCM 流服务，JY6316 以 16 kHz、PCM16 四槽模式运行，扬声器输出走 CH0。
Guest 自启动，先以 Codec 音量 75/100 播放 3 秒的 1 kHz 正弦波，再用同频 PCM
测试背压、低水位事件、双声道下混和 2 倍重采样。
只有 Guest 返回成功并有非零 PCM 提交给 xcodec 时才打印
`MicroPixel Orion audio Guest PASS`。实际扬声器声音需试听确认。

固件用原始 BIN 输出。用 [SDK 固件后处理流程](runtime.md#validation-executable)
处理副本，并用已核对的 APP 分区位置进行测试。`AUDIO_TEST=1` 独立运行 RV32F AOT 验证，综合 Host 使用 `HOST_PORT=1`。

| 组件 | Orion 上的用途 |
| --- | --- |
| `board_audio.h` + `xcodec_dev.h` | `xcodec_dev_board_init()` 初始化 JY6316、PA、共享 I²C 和 I²S/DMA，`xcodec_dev_open/read/write` 提供 PCM 录放，另有音量和静音接口。 |
| MicroPixel `AudioMixer`、`AudioOutputPeripheral` | 延续 Host 混音、Guest PCM 与 Ogg Opus 路径。在输出边界将立体声帧下混成 PCM16，再交给 xcodec。 |
| SDK `wave_player` / `audio_pcm` | 备选 SDK 音频前端，分别提供播放和 PCM 会话控制。使用时由单一前端持有编解码器与 DMA。 |
| SDK `OPUS`、`PVMP3DEC`、`AACDEC`、`OGG` | BL618DG 归档均为 ILP32D。完整 Host 使用 `PVMP3DEC` 对网易云音乐的 MP3 流解码，Ogg Opus 沿用 MicroPixel 的实现。 |

JY6316 包含 JY6311 DAC 与 JY6166 ADC。I²S 使用四槽 PCM16，扬声器写 CH0
（`channel_mask=0x01`），采样可先用 CH3（`0x08`）。用 16 kHz 进行录放回环。
DMA TX/RX 分别使用 `dma0_ch0` 和 `dma0_ch1`，显示配置应核对 DMA 分配。
录放共享同一条 I²S，双工时使用同一物理格式和采样率。关闭并删除录放设备后，
再释放板级数据接口。 [Orion 录放示例](https://github.com/bouffalolab/bouffalo_sdk/tree/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/examples/audio/xcodec_io)
[Xcodec 接口](https://github.com/bouffalolab/bouffalo_sdk/tree/9e34f621e6e3b0acef296fc5cedd0ed8c33d4464/components/multimedia/xcodec)

## Wi-Fi 与 BLE

从首页打开「配置」应用。该应用独立编译成 Wasm，通过 Wireless 1.1 Service 管理 Host 的无线状态。
只有内置配置应用有权提交无线命令。 [配置 Guest 源码](../../../guest/apps/settings/)

- **Wi-Fi** 页最多列出 SDK 返回的 100 个 AP，按 RSSI 从大到小排列。长网络名称可换行显示，每个 AP 标记各自的频段，
  同时扫到 2.4 GHz 和 5 GHz 的 SSID 标记为「双频」。点选网络后优先尝试同名、同为开放或加密的最强
  5 GHz AP，关联失败时尝试兼容的 2.4 GHz AP。开放网络可直接接入，加密网络需要密码。
  连上并获得 DHCP 地址后显示「Wi-Fi 已连接」，也可在此断开。最近一次成功连接的凭据保存到 LittleFS，供下次启动回连。
- **Bluetooth LE** 页扫描可连接设备，点选后发起连接。选择「允许发现」后，以
  `MicroPixel Orion` 广播并可被对端连接。连接后可在同一页断开。
  功能覆盖 BLE 链路建立与断开，GATT 应用协议和凭据传输由后续应用实现。

启动沿用 Timebox 的 Wi-Fi FHOST、lwIP 与 SYSINIT，由 SDK 事件向 Host 报告扫描结果和连接状态。
Orion BSP 复用 BL618DG DK 的 `board_rf.c` 和工厂 RF 参数。在 Wi-Fi 单无线构建中，
应用板级初始化在 FHOST 启动前将 BL-M65P-32S 模组内部的 SPDT 控制脚 GPIO16 拉低，
选中 2.4 GHz Wi-Fi 路径，再调用 DK 的 `board_rf_combo_init()` 加载并校准 PHY。
底板上的 RF_SWITCH 焊盘悬空，GPIO18 为触控复位。

| 配置 | 无线接口 |
| --- | --- |
| `RADIO=wifi` | 默认，启用 Wi-Fi STA 和 lwIP。 |
| `RADIO=ble` | 保留的 BLE 单无线构建，移植暂停。 |
| `RADIO=dual` | 保留的共存实验构建，需验证共享天线配置。 |

单无线固件的配置 Guest 会显示另一接口不可用。

Host 固件使用 SDK 原生 ILP32D，无线库、显示和音频在同一个 AP 核上运行。
ILP32F AOT 测试使用独立验证固件。默认 Wi-Fi 构建为天气、新闻和网易云音乐提供 HTTPS，
支持 JPEG/PNG 解码和 MP3 流式播放。天气更新由常驻 Service 发起。
