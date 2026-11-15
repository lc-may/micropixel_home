# D1–D3：PC 多应用基础交付

实现契约见[组件与资源](../design/app-components.zh-CN.md)，阶段范围见[九次交付计划](app-manager-plan.zh-CN.html)。
SDK 版本 0.21.0；本次只启用 PC 原生 Wasm 开发配置，不宣称 MCU 多实例或故障恢复完成。

| 交付 | 结果 |
|---|---|
| D1 | 每槽独立图形、字体、输入、HTTP 和媒体视图；PCM owner/completion sink、任务 owner 和局部清理 |
| D2 | 固定四槽 AppManager、最新前台意图、首帧交接、异步停止、容量拒绝、代次检查、包替换延迟释放 |
| D3 | Activity/AppService SDK、Lifecycle 1.0、组件资源域、requirements v2、v1 兼容、最小示例 |

```sh
bash tools/pc.sh build
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --multi-app
bash tools/tests/test_firmware_host.sh --pc --components
bash tools/tests/test_firmware_host.sh --pc
bash tools/tests/test_firmware_host.sh --pc --launcher
bash tools/tests/test_firmware_host.sh --pc --rv32
python3 -m unittest tools.tests.test_build_app_bundle_metadata tools.tests.test_store_publication tools.tests.test_micropixel_cli
bash tools/check_firmware_architecture.sh
```

`--app-framework` 使用无平台依赖的 owner、AppManager 和组件状态机，验证配额、过期代次、
命令容量、准备期限、最新意图与生命周期 ACK；另用确定的重入发布顺序覆盖 PCM 低水位唤醒后的重新 arm 竞争。该组通过标准 Host wrapper 执行，无需 esp-http3。

`--multi-app` 运行真实 Wasm：四槽准入、第五实例拒绝、独立 Activity/Service 资源、Service 停止后
页面仍有效，以及另一个 Service 存活期间 100 次启动/停止。独立音频上下文验证外国/过期 handle
拒绝、按 owner 暂停/关闭和 completion sink 路由；requirements parser 验证 v1/v2、畸形声明与旧环境拒绝。

`--components` 需要 X11/WSLg 与 loopback socket。自动启动两个组件 Guest 和一个旧式 Guest，验证
并发 HTTP、后台 PCM、输入只进前台、前后台纹理保持、旧应用安全挂起/恢复、页面销毁/重建、单独停止
Service 和整个 App。截图和 PCM 在本地 `build/pc-components-tests/`，协议负例在 conformance Guest 中执行。

全量 PC wrapper 覆盖六个既有 Guest、Scene/Raster/Direct Surface、网络/TLS/MQTT、市场、音乐和登录。
单槽 launcher 验证原音乐 Home 行为与替换应用；RV32 回归验证官方旧 AOT 和 IPC 边界。
PCM 通知竞争修复另通过 30 次真实 Guest 背压/低水位/关闭循环。
Bundle/CLI Python 回归覆盖 95 项。示例还生成 riscv32-ilp32f release Bundle，未改变 WAMR/wamrc 固定版本。

本地日志保存在 `build/d3-*.log`、`build/pc-test-results/`，不提交生成物、截图、PCM 或真实凭证。
格式检查使用项目 clang-format 配置，仅处理修改的 C/C++；完整 style wrapper 需要 ESP-IDF 环境。
未选择 MCU 板卡，不执行板级构建或硬件测试。RV32 多实例、正式音乐组件迁移、Widget/Message、Job 和 Alarm
分别继续按 D4–D9 接入。正常回收通过不等于纯死循环可限时取消或原生崩溃可恢复。
