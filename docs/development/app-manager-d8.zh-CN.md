# D8：RV32 多实例与混合后端

在仓库根目录运行，准备好 PC 依赖、WASI SDK、固定版本 wamrc、QEMU RV32 以及带
`rv32imafc/ilp32f` Newlib 的裸机 GCC。`WAMRC` 可显式指定固定版本工具；已有本地工具时
`pc.py` 自动发现。不会升级 WAMR，也不需要修改官方 AOT。

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home --rv32
```

run 自动构建 RV32 runner，以及天气、时钟、音乐的 release Bundle。Home 冷启动天气和时钟
Service，点击音乐进入登录/列表；按 Home/F1 返回时音乐后台播放。默认不加 `--rv32` 时使用
原生 Wasm。`--single-app` 保留单槽；`--multi-app` 额外开放组件演示的 task 开发配置。

可分别构建和组合不同后端；同一 AppId 只加载一个后端版本：

```sh
python3 tools/pc.py guest/apps/netease-music --backend rv32
python3 tools/pc.py guest/apps/weather
python3 tools/pc.py guest/apps/clock-alarm
bash tools/pc.sh run --home \
  build/pc-rv32-apps/netease-music/netease-music.pc.json \
  build/pc-apps/weather/weather.pc.json \
  build/pc-apps/clock-alarm/clock-alarm.pc.json
```

RV32 产物位于 `build/pc-rv32-apps/<app>/`，包含 `.wasm`、`.aot`、`.bundle.bin` 和
`.pc.json`。描述文件引用 Bundle；运行时身份和能力以已校验的 Bundle 元数据为准。
原生输出仍在 `build/pc-apps/`。生成物不提交源码仓库。

| 能力 | 原生 Wasm | 新 RV32 checked AOT | 官方旧 RV32 Bundle |
|---|---|---|---|
| 同一 AppManager、最多四槽、可混合运行 | 支持 | 支持，另受进程/内存预算约束 | 支持 |
| Home 返回原实例、输入与画面归属 | 支持 | 支持 | 支持，隐藏时挂起 |
| Activity/Service 与后台音频 | 按声明 | 按声明 | 仅 LegacyActivity，无后台 Service |
| Widget、消息与冷启动权限 | 按声明 | 按声明 | 无声明不提供 |
| Job、Host 闹钟、焦点 | 按声明 | 按声明 | 无声明不提供 |
| 单实例停止、包替换、资源排空 | 支持 | 另回收私有 QEMU/RAM/socket | 同 RV32 |
| 自动故障恢复、RTOS/no-MMU 多应用 | 本阶段不交付 | 本阶段不交付 | 本阶段不交付 |

框架设计与额度定义见 [RV32 后端](../design/pc-rv32.zh-CN.md)，交付范围见
[D8 计划](app-manager-plan.zh-CN.html#d8)。D9 的目标板适配单独进行。

统一验证入口：

```sh
# 四槽混合实例、私有 RAM/PID、独立 PCM、反复重启、单个子进程异常退出。
bash tools/tests/test_firmware_host.sh --pc --rv32-multi
# 构建并运行同一套业务断言、两个方向的混合后端、官方旧包与非法 IPC。
bash tools/tests/test_firmware_host.sh --pc --rv32-suite
```

模型测试使用虚拟时间验证 Job/Alarm，实际 QEMU 验证天气冷/热/挂起唤醒、HTTP 失败与截止、
Widget/消息身份、Clock 持久登记与清理、音乐登录与列表/收藏/推荐/播放，以及后台自动下一首。
测试网络使用 localhost fixture；真实手机授权需本人操作。截图、PCM、日志保存在本地 `build/`。

本次验证已分批通过：四槽混合与实例隔离、原生 100 次启动/停止、组件与 HTTP/PCM 回归、
RV32 生命周期/Widget/消息边界、天气 Job、Clock 持久闹钟、三应用焦点、音乐播放/扫码状态/
主页/收藏/滚动推荐/重叠刷新，以及两个方向的 35 秒长请求与后台自动下一首。
官方固定旧包通过渲染、触摸、非静音音频、三次启停、市场按钮安装、返回 Home 复用实例及非法 IPC。
`pc.sh run --home --rv32` 的真实入口也已用独立测试存储验证，截图可见时钟和天气卡片。

构建元数据 17 项与 CLI 73 项测试、架构检查、文档链接和修改文件的 clang-format 检查通过。
正式固件格式包装器要求 ESP-IDF，本环境未设置 `IDF_PATH`，因此使用已有 clang-format 单独检查；
未进行 D9 目标板构建或硬件验证。
