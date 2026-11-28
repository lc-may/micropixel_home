# Orion 板端应用管理

生产 AOT Host 使用本机 C 编译器和 SDK 的 LittleFS 生成出厂镜像，在独立的 LittleFS 卷中安装 Bundle v1。卷最多容纳 15 个应用，
应用的实际容量取决于压缩后的总大小和 LittleFS 开销。升级会先保留旧版文件，
新文件写完后再原子替换。用户需留出新包和文件系统所需的空间。

首装时烧入出厂 App Store 镜像，预装配置、日历与提醒、天气、网易云音乐和新闻。
支持 A2DP 的 Orion 固件还预装蓝牙音箱。
出厂应用使用与后装应用相同的文件格式和安装目录。
以后只更新 Host 固件时保留应用卷，设备重启也保留卸载结果。

诊断 Bundle 通过 `cmake --build <Orion 构建目录> --target orion_diagnostics` 单独构建，随后可从串口安装。

板端「应用管理」显示版本与空间，负责确认从物理 UART 导入的安装、升级及卸载。
上传前先构建 Orion RV32 ILP32D Bundle。开发者可从同一个 UART 管理接口确认操作。
升级要求同一 AppId 与更大的版本号。目标应用退出后才修改其安装文件。

物理 UART 是本地开发与维护入口，操作者负责确认 AOT 包来源。Wasm 解释器验证配置
沿用其内置调试目录。P4/S31 的 ILP32F 包和 S3 Xtensa 包使用各自的构建目标。

## 导入与卸载

从仓库根目录构建并传输应用，串口使用 Orion 的 2 Mbps UART。CLI 使用
[`requirements-dev.txt`](../../../requirements-dev.txt) 中的 pyserial。

```sh
python3 tools/micropixel package guest/apps/weather --aot-target riscv32-ilp32d --output-dir build/orion-weather
python3 firmware/bouffalo/tools/orion_apps.py --port /dev/ttyACM0 list
python3 firmware/bouffalo/tools/orion_apps.py --port /dev/ttyACM0 import build/orion-weather/weather.bundle.bin
python3 firmware/bouffalo/tools/orion_apps.py --port /dev/ttyACM0 remove micropixel.weather
```

在 Orion 的「全部应用」打开「应用管理」。传输结束后确认安装或升级，也可以选择
列表中某个应用的「卸载」。串口 `apply` 与触屏确认执行同一操作。
默认镜像中的同版本应用可在卸载后重新安装，直接升级需先提高应用清单版本号。

## 应用私有数据

每个已安装的 Bundle 在 `mp_apps` 的 `/.data/<AppId>/` 下拥有一个目录。Host 使用当前会话的 Bundle AppId 定位目录。
Guest 通过 SDK 的 `Application::storage()` 读写或删除本应用的键。每个键对应一个文件，键名最长 15 个 ASCII 字符，允许字母、数字、
点、连字符和下划线，单独的 `.` 和 `..` 保留给文件系统。Guest 每个值最多 4 KiB，每个应用最多 16 个键、共 16 KiB。
每个会话可以连续写入或删除 8 次，之后每秒恢复一次额度。应用可在键名中标注数据版本，例如 `playback.v1`。

Host 在安装和首次识别已安装 Bundle 时创建目录。写入关闭临时文件后以重命名原子替换原值。
升级或卸载 Bundle 时保留该 AppId 的资料，重新安装可恢复。应用包与资料共享 `mp_apps` 的可用空间。
更新固件时保留应用卷，重刷出厂应用卷会重置已安装包及其中的资料。

固件启动时从旧 `mp_kv` 文件迁移天气缓存 `weather.v1`、日历事项 `clock.notes`、闹钟记录 `clock.alarms.v1` 和音乐账号 `ncm.auth`。
已经存在的新位置优先，成功提交后清理旧记录。日历事项在 `micropixel.clock-alarm/notes.done`，Host 管理的时钟及闹钟数据在同一目录的
`@alarms.v1`。Wi-Fi 系统配置继续保存在 `mp_kv`。解释器验证配置使用 `mp_kv` 中的同一目录约定。

## 容量

`mp_apps` 为 4 MiB。每包上限为压缩前 2 MiB、压缩后 768 KiB。15 个压缩后
平均为 200 KiB 的包占约 3 MiB，余量供应用资料、待提交的新版本和文件系统元数据使用。
超过卷内可用空间时安装返回 `no_space`，已安装版本继续可用。单个应用升级或卸载时，
其他 Guest 会话可继续使用。

应用可通过 Resource 服务加载 Bundle 图片。Guest 字体组件、Ogg/Opus 音频资源和联网分发取决于 Orion 现有 Host 能力。
