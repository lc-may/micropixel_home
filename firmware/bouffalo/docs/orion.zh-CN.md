# BL618DG Orion 外设接入

## PC Host 移植

`HOST_PORT=1` 在 Orion 的 BL618DG B1 芯片上使用 AP 核运行 FreeRTOS Host。
板级构建选择 `CPU_MODEL=b1`。完整 Host 使用 SDK 预编译无线库和 MP3 解码库的原生 ILP32D ABI。
应用和 conformance Guest 通过与 PC 相同的 `tools/pc.py` 构建，默认用固定版本的 `wamrc`
编译为 RISC-V ILP32D AOT v6。
WAMR 沿用 Core ABI、`ServiceRegistry`、`EventQueue` 和 `AppManager`，固件提供四个 Guest 槽。
Guest 在实例化时按应用上限预留 PSRAM，支持时钟、日志、事件等待、硬件随机数、语言和启动参数，
以及定时器、合成音效和 PCM 流。定时器支持一次触发、周期、取消和销毁，Guest 在
`event_wait` 取走到期事件，周期跨过多次截止时间时合并为一次事件。
配置、日历与提醒、天气、网易云音乐、新闻，以及支持 A2DP 时的蓝牙音箱各自打包为 Bundle v1，
压缩后写进出厂 `orion_app_store.bin`。Host 从 LittleFS
应用卷读取安装记录，在 PSRAM 中解压、校验、重定位并执行。AOT 应用停止采用协作事件。`ORION_AOT=0` 构建带指令预算的
解释器固件，应用字节码随固件嵌入。

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion HOST_PORT=1 \
    BL_SDK_BASE=/path/to/clean/bouffalo_sdk
```

将 WASI SDK 和 [Bouffalo 构建工具链](../README.md)准备好后，从仓库根目录构建。
把固定版本的 WAMR `wamrc 2.4.3` 加入 `PATH`，或通过环境变量 `WAMRC` 指定。
Orion Host 的云端文字字库在构建时使用 Pillow 和 fontTools 生成，构建会下载并校验固定版本的思源黑体。
离线构建可通过 `ORION_FONT_SOURCE=/path/to/SourceHanSansSC-Regular.otf` 指定同一版本的本地文件。
默认 `RADIO=dual`，构建产物在 `build-orion-host-dual/build_out/micropixel_orion_host_bl618dg_ap.bin`。
烧录前用仓库工具准备固件镜像。它会补齐仅执行 `cmake --build` 时缺少的 SDK 证书包步骤，
并在加入 Orion 的 RF 参数后校验证书包。`BL_SDK_BASE` 指向构建使用的 SDK 绝对路径。

```sh
python3 firmware/bouffalo/tools/prepare_orion_firmware.py \
    firmware/bouffalo/boards/bl618dg/build-orion-host-dual /tmp/micropixel_orion_host.bin
```

按 [SDK 烧录流程](runtime.md#validation-executable) 检查设备、分区表与容量。
首次部署时，将同一构建目录下的 `orion_app_store.bin` 写入 `mp_apps`（`0x400000`），
处理后的固件写入 `FW` 主槽。日常更新 Host 保留 `mp_apps`。通过串口的
`mpx bundle list` 和 `MPX LOAD ... backend=aot` 确认应用已安装和启动。
[应用管理与导入命令](app-store.zh-CN.md)
首页中排左侧是「全屋灯光」，右侧是「中央空调控制器」。对应应用安装后即可从卡片启动，未安装时卡片显示「未安装」。
点击首页时间仍可打开日历与提醒，应用也可从「全部应用」打开。
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
主闹钟与各事项的提醒时间、重复规则、启用状态和上次触发记录保存在 `/.data/micropixel.clock-alarm/@alarms.v1` 中。
文件同时保存时区和时钟设置，开机恢复记录，校时后重新计算下次触发。事项完成状态保存在同一目录的 `notes.done`。

`boards/bl618dg/orion/` 定义 LCD、触控、音频引脚和 Host 组合入口。
`runtime/` 提供 FreeRTOS Guest Runner、标准 Core imports 及服务端点，复用共享的
`AppManager`、`ServiceRegistry`、`EventQueue` 和 PCM 流实现。

| PC 功能 | Orion 接入 |
| --- | --- |
| 原生 Wasm、应用目录与生命周期 | 触控和串口操作已安装目录，复用 `AppManager` 管理四个 Session。首页可启动配置、闹钟、天气、网易云音乐和新闻。媒体 Widget 和点对点播放控制复用 `AppHub`。 |
| SDL 显示和触控、Home 与应用 UI | Orion 和 PC 共享 Home 的布局、配色和 Material 图标，480×480 LVGL 对接 ST7102 与 ST7123。前台 Guest 支持逻辑画布缩放、文字、裁剪、动态纹理和 JPEG/PNG 解码。天气和音乐 Widget 接收各自后台 Session 的最新快照。 |
| 文件和 SQLite、资源包、本地安装 | `mp_kv` 保存 Wi-Fi 等系统配置。`mp_apps` 保存已安装的 Bundle 和按 AppId 隔离的 Guest KV。详见[应用私有数据](app-store.zh-CN.md#应用私有数据)。 |
| POSIX 网络、HTTPS/MQTT、市场及联网 Guest | 配置 Guest 管理 Wi-Fi STA。Network 1.3 提供活动和后台 Service 两条 HTTPS 通道，支持 GET、POST、响应头和接收超时。明文 HTTP、MQTT 和联网市场待接入。 |
| 音效、PCM、压缩音频和 URL 媒体 | 音效与 PCM 输出和 LCD 可共用同一固件。网易云音乐的 HTTPS MP3 走 Host 混音器，支持暂停、进度跳转和后台播放。录音及其他 URL 音频格式待接入。 |
| RV32 AOT | Orion 生产目录使用 SDK 的 ILP32D。公共 ILP32F Bundle 在独立验证固件运行。 |

## 内存与启动性能

Orion 默认开启 `CONFIG_PSRAM_COPY_CODE`。SDK 启动阶段从 Flash 复制常规 Host 指令和只读数据，
完成搬运后才启动 Host。启动、时钟切换和 Flash 操作所需的专用代码段按芯片要求链接。
Guest 镜像在 PSRAM 中解压、装载和执行，模块、实例与线性内存也分配在 PSRAM。
AOT 装载完成并确认运行时已接管所需数据后，释放临时解压镜像。

OCRAM 存放 FreeRTOS 任务栈和片内全局变量。WRAM 供 lwIP 内存池、Wi-Fi 数据和无线 SDK 堆使用。
Orion Host 的通用堆与 FreeRTOS 动态对象走 PSRAM。帧缓冲、字库、场景、XZ 工作区和应用数据也在 PSRAM。
Session 进入后台时释放像素缓冲，回到前台后用保留的场景重新绘制。

每个启动的应用按模块实际初始页数加上增长额度预留连续 PSRAM。Wasm 静态数据，包括大型全局变量，
由编译器计入初始页数。线性内存按 64 KiB 一页增长，增长不搬动基址，应用停止后释放其预留区。
模块声明的最大页数也会限制增长。Host 图形资源和网络工作区从共享 PSRAM 分配。

| 应用 | 当前初始线性内存 | 增长额度 | 当前预留上限 |
| --- | ---: | ---: | ---: |
| 网易云音乐 | 192 KiB | 960 KiB | 1152 KiB |
| 闹钟 | 256 KiB | 1152 KiB | 1408 KiB |
| 天气、新闻 | 256 KiB | 768 KiB | 1024 KiB |
| 配置 | 192 KiB | 128 KiB | 320 KiB |
| 诊断应用 | 64 KiB | 0 KiB | 64 KiB |

`MPX LOAD` 的 `linear`、`limit` 和 `reserved` 分别报告初始大小、有效上限和 PSRAM 分配大小。

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
选择内置 Guest 的优化级别。烧录前核对 SDK 后处理后的完整镜像能否装入目标 FW 槽。

## 8 MiB Flash 分区

Orion Host 指定 [partition_cfg_8M.toml](../boards/bl618dg/orion/config/partition_cfg_8M.toml)。
它适用于 JEDEC 容量为 8 MiB 的设备。主固件槽、设置和工厂数据沿用原地址，
上半区用于应用卷。

| 区域 | 起始地址 | 容量 |
| --- | --- | --- |
| Boot2 和双份分区表 | `0x000000` | 64 KiB |
| FW 主槽 | `0x010000` | 3812 KiB |
| mp_kv（LittleFS） | `0x3c9000` | 128 KiB |
| PSM、KEY、DATA、factory | `0x3e9000` | 92 KiB |
| mp_apps（LittleFS） | `0x400000` | 4096 KiB |

此布局将以前的 FW 备用槽、`imgres` 和只读 `apps` 区域合并为 `mp_apps`。
迁移前备份整片 Flash，确认主槽为启动槽。采用上一版 8 MiB 布局时可保留
`mp_kv` 及系统和工厂数据。使用旧版 `0x7e0000` 的设置分区时，先从备份迁移设置到
`0x3c9000`。烧录并读回校验新的主固件与出厂应用卷后，更新双份分区表。
从出厂镜像开始，应用升级和卸载由设备端应用管理完成。

## Wi-Fi 回连与天气刷新

Orion 从 `mp_kv` 挂载 LittleFS。Wi-Fi 获得 IPv4 后，Host 将 SSID、密码、BSSID 和信道写入
`settings.wifi.v2`，同一 SSID 和 BSSID 的记录会更新并移到首位。开机及掉线时，Host 在后台扫描，按最近
成功连接的顺序尝试扫到的历史 AP。一条候选关联或获取 IPv4 失败后，继续尝试同一次扫描的下一条候选。
没有可用候选时，每隔 120 秒重扫。手动扫描、连接或断开后，自动回连最多等待 60 秒，给用户留出配网时间。
DHCP 模式在每次连接后重新取得地址。静态模式按 AP 保存 IPv4、掩码、网关和 DNS，并在回连时恢复。
配置应用的连接入口使用 DHCP。现有 SDK 串口命令 `set_ipv4 <ip> <dns> <gateway> <netmask>` 可为已连接的 STA 设置静态值。
写入先关闭临时文件，再重命名提交。已有 `settings.wifi` 单条记录会自动导入，已有 Timebox 配置在首次成功连接后迁移。

拿到 IP 后，Host 通过 SNTP 校时，成功后每 15 分钟重校。每次校时成功都会更新 TLS、Home 和闹钟的 UTC 基准。
网络中断期间，Home 和闹钟继续使用最近一次 UTC 基准与单调时钟走时。串口手动设置的 UTC 基准在下一次成功校时时更新。
天气应用通过 Network Service 请求 Open-Meteo，应用启动、切换城市及点击刷新时发起更新。
天气 Service 在开机后占用独立 Session。首次启动先从 LittleFS 的 `/.data/micropixel.weather/weather.v1` 加载城市与最近一次天气，
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

## Wi-Fi、BLE 与经典蓝牙

从首页打开「配置」应用。该应用通过 Wireless 1.4 Service 管理 Host 的无线状态。
经典蓝牙使用独立的 Bluetooth Service。内置 `micropixel.settings` 应用拥有这两个服务的无线控制权限。 [配置 Guest 源码](../../../guest/apps/settings/)

- **Wi-Fi** 页最多列出 SDK 返回的 100 个 AP，按 RSSI 从大到小排列。长网络名称可换行显示，每个 AP 标记各自的频段，
  同时扫到 2.4 GHz 和 5 GHz 的 SSID 标记为「双频」。点选网络后优先尝试同名、同为开放或加密的最强
  5 GHz AP，关联失败时尝试兼容的 2.4 GHz AP。开放网络可直接接入，加密网络需要密码。
  连上并获得 IPv4 地址后显示「Wi-Fi 已连接」，也可在此断开。Host 会记住成功连接过的 AP，退出配置应用后仍可自动回连。
- **BLE** 页扫描最多 16 台可连接的 BLE 设备，并显示名称、地址和信号强度。扫描最多 6 秒，也可提前结束。
  点选设备可连接或取消连接。选择「允许发现」后，Orion 以 `MicroPixel Orion` 广播并接受一条对端连接。
  对端断开后会恢复广播。「停止广播」在连接期间也可用，关闭发现并保留当前连接。「断开连接」有独立按钮。
  PC 连接后可从 GATT Device Information Service（`0x180A`）
  的 Model Number（`0x2A24`）读到 `BL618DG Orion`。

启动沿用 Timebox 的 Wi-Fi FHOST、lwIP 与 SYSINIT，由 SDK 事件向 Host 报告扫描结果和连接状态。
Orion BSP 复用 BL618DG DK 的 `board_rf.c` 和工厂 RF 参数。在 Wi-Fi 单无线构建中，
应用板级初始化在 FHOST 启动前将 BL-M65P-32S 模组内部的 SPDT 控制脚 GPIO16 拉低，
选中 2.4 GHz Wi-Fi 路径，再调用 DK 的 `board_rf_combo_init()` 加载并校准 PHY。
底板上的 RF_SWITCH 焊盘悬空，GPIO18 为触控复位。

| 配置 | 无线接口 |
| --- | --- |
| `RADIO=wifi` | 启用 Wi-Fi STA 和 lwIP。 |
| `RADIO=ble` | BLE 单无线构建。GPIO16 拉高选择 BT 路径，沿用 BL618DG DK 的 standalone PHY 校准。 |
| `RADIO=dual` | 默认，启用 Wi-Fi STA、经典蓝牙 BR/EDR 和 BLE。采用 standalone 单天线 SPDT 拓扑，GPIO16 由硬件 PTA 控制。 |

单无线固件的配置 Guest 会显示另一接口不可用。

Host 固件使用 SDK 原生 ILP32D，无线库、显示和音频在同一个 AP 核上运行。
ILP32F AOT 测试使用独立验证固件。默认构建为天气、新闻和网易云音乐提供 HTTPS，
支持 JPEG/PNG 解码和 MP3 流式播放。天气更新由常驻 Service 发起。

BLE 和经典蓝牙的扫描结果按「鼠标、键盘和笔」「音频」「其他设备」分组。BLE 使用广播和扫描响应中的
Appearance 与服务 UUID，经典蓝牙使用 Class of Device 和扩展查询响应中的服务 UUID。
信息不足的设备归到「其他设备」。 [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Assigned_Numbers/out/en/index-en.html)

### 经典蓝牙与 A2DP

默认的 `RADIO=dual` 固件提供经典蓝牙页面。使用「配置」0.1.6，页面可扫描和连接设备、开启发现、配对和取消配对。
需要更换角色时，先断开经典蓝牙连接并关闭发现，然后点击页面右侧的角色按钮。一台 Orion 同一时间运行一个 A2DP 角色。

- **接收 · Sink** 是上电默认角色。在 Orion 打开「允许发现」，从 Android 或 Windows 配对并将音频输出选为 `MicroPixel Orion`。在对端控制播放，Orion 扬声器播放接收到的 SBC 音频，并显示协商采样率。
- **发送 · Source** 可连接处于配对模式的蓝牙音箱或耳机。扫描后点选设备，连接 ACL 后点「连接音频」。音频开始传输后，Orion 上通过系统音频服务播放的声音会送到对端。「暂停音频」和「开始音频」控制 A2DP stream。

授权 Guest 使用 `sdk/bluetooth.hpp` 打开 Bluetooth Service。控制方法入队后通过 `Status` 读取完成状态和 SDK 错误码。
扫描结果按 `scan_revision` 读取。配对、查看绑定和取消配对使用经典蓝牙地址。当前 SDK 配置将经典蓝牙绑定保存在运行内存中。

默认音频路由 `MPX_BT_AUDIO_SYSTEM` 连接 Host 混音器。切换到 `MPX_BT_AUDIO_GUEST` 后，
Sink 可通过 `ReadPcm` 取出已解码音频，Source 可通过 `WritePcm` 提供待编码音频。
使用 `Status` 返回的 `stream_revision`、采样率和声道数传输交错的有符号 16 位 PCM。
单次最多 1024 字节，按完整采样帧读写。队列暂时为空或写满时返回 `WOULD_BLOCK`，应在 Guest 事件循环中重试。
音频编解码使用 SBC。 [Guest ABI](../../../guest/abi/bluetooth.h) [C++ SDK](../../../guest/sdk/bluetooth.hpp)

出厂「蓝牙音箱」通过 Bluetooth Service 读取经典蓝牙状态和控制 AVRCP，支持从「全部应用」启动和后台播放。
Bluetooth 1.3 新增 `RemoteTrack()`，返回当前 AVRCP 会话的曲名、歌手和专辑。各字段为 UTF-8，音源未提供的字段为空。 [蓝牙音箱 Guest](../../../guest/apps/bluetooth-speaker/README.zh-CN.md)

### 用 PC 验证 BLE

`ORION_BLE_PROFILES=1` 在 Orion Host 上启用 Battery Service（`0x180F`）、Heart Rate Service（`0x180D`）
和 Orion GATT 回环。Battery Level（`0x2A19`）支持读和通知，Heart Rate Measurement（`0x2A37`）支持通知。
服务使用模拟数据，供 Windows Orion BLE 测试台执行 Profile 自检。回环支持写请求、写命令、通知和回读。
Windows 工具位于工作区的 `orion_ble_tester`，打开 `dist/OrionBleTester.exe` 即可扫描和连接。

```sh
make -C firmware/bouffalo/boards/bl618dg BOARD=bl618dg_orion HOST_PORT=1 \
  RADIO=dual ORION_BLE_PROFILES=1
```

用本页开头的镜像准备工具处理 `build-orion-host-dual-profiles`。烧录后在「配置 → 蓝牙」开启发现，
也可通过 UART 使用 `orion_ble advertise`。广播包含 Device Information、Battery 和 Heart Rate UUID。
5 GHz Wi-Fi 与 BLE 使用同一共存配置，GPIO16 由硬件控制。

在 Linux 或 WSL 中设置 `BL_SDK_BASE`，指向按 [SDK 说明](sdk.md) 准备的 Orion SDK。
其中的 [Orion BSP 补丁](../patches/orion-bt-standalone-spdt.patch) 为 BLE 单无线构建配置 GPIO16。
SDK `peripheral` 示例和 Orion BLE 单无线 Host 共用这一初始化。

在 MicroPixel 仓库根目录先构建 SDK `peripheral` 示例。

```sh
repo="$PWD"
test_build="$repo/build/orion-ble-smoke"
cmake_definition="-DCMAKE_PROJECT_INCLUDE=$repo/firmware/bouffalo/cmake/orion_ilp32d.cmake" \
make -C "$BL_SDK_BASE/examples/btble/peripheral" ninja \
  CHIP=bl618dg BOARD=bl618dg_orion CPU_ID=ap CPU_MODEL=b1 \
  CROSS_COMPILE=riscv64-zephyr-elf- CONFIG_DUALCORE_DISABLE=y CONFIG_PSRAM=y \
  CONFIG_BTBLE_USE_STANDALONE_PATH=y CONFIG_CONSOLE_UART_BAUDRATE=2000000 \
  CONFIG_SKIP_COMBINE=y BUILD_DIR="$test_build/sdk-peripheral"
```

为 `sdk-peripheral/build_out/peripheral_bl618dg_ap.bin` 的副本运行 SDK 的 `bflb_fw_post_proc`。
指定 `--chipname=bl618dg --appkeys=shared`，`--dtsfile` 使用 SDK 的
`bsp/board/bl618dgdk/config/bl_factory_params_IoTKitA_auto.dts`。读取并备份板上主 FW 槽，核对芯片 MAC 和分区表后，
只将测试镜像写入已确认的主 FW 槽 `0x10000`。串口以 2000000 波特率查看
`BT standalone RF switch on GPIO16 selected`、`BD_ADDR` 和 `Start advertising success.`。

使用 `RADIO=ble ORION_BLE_SMOKE=1` 构建 MicroPixel 诊断固件。

```sh
make -C firmware/bouffalo/boards/bl618dg ninja BOARD=bl618dg_orion HOST_PORT=1 \
  RADIO=ble ORION_BLE_SMOKE=1 BUILD_DIR="$test_build/micropixel"
python3 firmware/bouffalo/tools/prepare_orion_firmware.py \
  "$test_build/micropixel" "$test_build/micropixel-ble.bin"
```

诊断固件启动后，在设置中选择 Make discoverable，或在串口执行 `orion_ble advertise`。
`orion_ble hide` 停止广播，`orion_ble scan` 扫描附近可连接设备，`orion_ble disconnect` 主动断链。
这些命令复用 Host 的 Wireless 命令队列。诊断构建还注册一条最大 20 字节的 GATT 特征值，支持读取、写入和通知。

在有蓝牙适配器的 PC 上安装 `bleak`，从仓库根目录运行 [对端测试脚本](../../../tools/test_orion_ble.py)。
使用 WSL 时在 Windows 原生 Python 中运行脚本，访问 Windows 管理的蓝牙控制器。

```sh
python -m pip install bleak
python tools/test_orion_ble.py sdk --address "$ORION_BLE_ADDR"
python tools/test_orion_ble.py micropixel
```

将 `ORION_BLE_ADDR` 设为示例串口打印的 `BD_ADDR`。
示例验证广播、GAP 连接和断开、GATT 发现、读、写、通知、指示。MicroPixel 验证同一条特征值的写入、通知和回读。
从 PC 断开后，MicroPixel 可再次执行 `orion_ble advertise` 测试重连。测试结束后可将备份写回主 FW 槽并读回校验。

### 5 GHz Wi-Fi 与 BLE 共存

`RADIO=dual` 选择 `CONFIG_COEX_BOARD_TOPOLOGY_STANDALONE_SINGLE_ANT_SPDT` 和
`CONFIG_COEX_BOARD_SPDT_GPIO=16`。BL-M65P-32S 的 SPDT 在 2.4 GHz Wi-Fi 和 BT RF 之间切换。
5 GHz Wi-Fi 通过 diplexer 接到同一根天线。共存时 GPIO16 复用为 `GPIO_FUNC_SPDT`，由 PTA 决定输出。
SDK 切换到 5 GHz 共存配置时，Orion BSP 保持这项硬件复用。

```sh
test_build="$PWD/build/orion-coex-smoke"
make -C firmware/bouffalo/boards/bl618dg ninja BOARD=bl618dg_orion HOST_PORT=1 \
  RADIO=dual ORION_BLE_SMOKE=1 BUILD_DIR="$test_build/micropixel"
cmake --build "$test_build/micropixel" --target post_build
python3 firmware/bouffalo/tools/prepare_orion_firmware.py \
  "$test_build/micropixel" "$test_build/micropixel-coex.bin"
```

板端连到 5 GHz AP 后，串口用 `wifi_sta_info` 确认信道和 IP，用 `wifi_coex_status` 查看 5 GHz hardware-only 状态。
`memtrace 0x20000904` 可读回 GPIO16 的复用配置。寄存器值与 `0xc0001f40` 按位与后应得到 `0x40001900`。
运行 `orion_ble advertise`，在 PC 持续 ping Orion 的同时执行 `python tools/test_orion_ble.py micropixel --rounds 30`。
检查 BLE 连接期间的 Wi-Fi 回包、GATT 写入通知与回读，以及连接前后的 GPIO16 复用和 Wi-Fi 信道。

需要在 15 路 TCP 满载时测 GATT 吞吐，可同时启用 `ORION_BLE_SMOKE=1` 和 `ORION_RX_BENCHMARK=1`。
诊断服务 `79180b9c-f9dc-47b2-a71b-4cd98f201203` 支持连续 Write Command 和 Notification，
每帧上限为协商 ATT MTU 减 3 字节，最多 244 字节。每帧前 4 字节是小端序号，余下字节为 `0xa5`。

| 特征值 UUID 后四位 | 操作 |
| --- | --- |
| `1204` | PC 向 Orion 发 Write Command |
| `1205` | Orion 向已订阅的 PC 发 Notification |
| `1206` | 写单字节控制值。`1` 启动上行，`2` 启动通知，`3` 启动双向，`0` 停止，`4` 请求较短连接间隔、251 字节数据长度和 LE 2M PHY |

每轮序号从 0 开始。串口 `gatt_bench_stats` 记录板端收包、序号缺口、无效载荷和通知提交计数。
读取 `1206` 会返回 4 字节小端已收帧数，PC 可据此限制无响应写入的在途队列。
上行以板端有效收字节计，下行以 PC 实收且通过序号和载荷校验的通知字节计。
