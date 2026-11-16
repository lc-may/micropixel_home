# 家庭面板运行与验证

界面参考与功能边界见[设计](../design/panel-ui.zh-CN.md)。新界面替换原 Home、天气和日历闹钟的页面，
保留 Activity/Service、Job、Widget、消息、Host 闹钟与 RV32 多实例结构。

## 运行

在仓库根目录执行：

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home
# 使用相同 SDK 构建的 RV32 checked AOT。
bash tools/pc.sh run --home --rv32
```

Esc/Home/F1 返回面板，音乐继续播放，登录和队列保留；点击音乐卡片返回原实例。
Home 再按 Esc 保持主页；应用目录中按 Esc 返回主页。进入目录点击“关闭当前应用”才停止最近的当前应用，其他后台应用继续运行。
时间卡进入闹钟，天气区进入天气；“日历与事项”卡片进入月历，
“应用”进入目录、市场、音量和关闭操作。事项在日历/事项页添加：输入名称、选择未来日期和时间、保存。
系统确认后 Home 显示下一条事项，关闭 Guest 也保留提醒。

```sh
bash tools/pc.sh run --home --screenshot build/home-panel.png
```

按 F12 保存当前界面，退出时再次保存。生成的 PNG、PCM、日志和 Bundle 均留在忽略的 `build/`。

## 自动验证

```sh
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --panel
bash tools/tests/test_firmware_host.sh --pc --panel --rv32
```

`--panel` 构建 Host 和所选后端的三个应用，使用独立临时数据库、本地 HTTP 音乐/天气/授权 fixture，
在真实 SDL/X11 窗口执行交互，检查实际截图与 SDL 输出的非静音 PCM。无需真实音乐账号。

| 验证 | 覆盖范围 |
|---|---|
| 日历模型 | 与操作系统公历日期对照 48000 天，闰年/世纪年、星期、月份长度、时区和跨年 |
| 事项持久化 | 真 Guest 输入中文名称并保存，停止应用、重建 Host，Guest 未启动时提醒；完成和删除 |
| Home | 数字时钟主卡和四块功能卡片、天气更新、闹钟/事项摘要、应用目录与后台音乐 |
| 天气 | 10 个常用城市、34 省区/394 城市地区，省区和城市滚动不误选，北京/乌鲁木齐请求坐标与重启恢复；小时/三日预报、湿度/风速、Material 天气图标与失败缓存 |
| 日历/事项 | 月历、闹钟设置入口、编辑名称与时间、保存、完成/恢复、闹钟保存 |
| 音乐 | 模拟授权后读取个人内容，推荐、播放/暂停、拖动松手跳转、红星确认后的实际颜色 |
| 生命周期 | Esc 返回 Home 后 PCM 持续输出，Home 暂停/恢复有效，退出无 Guest 错误 |

面板截图位于 `build/panel-tests/` 和 `build/panel-rv32-tests/`，包括 `home.png`、`home-reminder.png`、
`home-music.png`、`music-list.png`、`music-liked.png`、`weather.png`、`calendar.png` 和 `notes.png`。

面板原生 Wasm 和 RV32 AOT 的交互及事项持久化测试均已通过；AppManager/Widget/消息/Job/Alarm
模型、公历对照、天气解析与 32 个 Material 图形展开测试通过。本轮以下相关回归通过：

```sh
bash tools/tests/test_firmware_host.sh --pc --home-widgets
bash tools/tests/test_firmware_host.sh --pc --alarms
bash tools/tests/test_firmware_host.sh --pc --jobs
bash tools/tests/test_firmware_host.sh --pc --market --interactive
bash tools/tests/test_firmware_host.sh --pc --launcher
bash tools/tests/test_firmware_host.sh --pc --launcher --rv32
```

闹钟回归包含停止 Clock Guest 后响铃、弹窗输入与音频焦点；Job 回归覆盖仅含 current 的旧天气响应、
后台更新与持久停止策略。面板测试覆盖当前/预报数据、晴/阴/雾/雨/雪/雷暴图标及 503 时保留缓存。
启动器回归覆盖原生/RV32 的 Esc 退后台、连续播放与自动下一首、Home 再按 Esc、登录与实例复用；
Widget 回归验证显式关闭天气不会停止后台音乐，重新进入音乐后可显式关闭该实例。
修改文件的 clang-format、Python/Bash 语法、文档相对链接、`git diff --check` 和固件架构检查通过。
正式固件格式包装器需要已激活的 ESP-IDF，本环境未设置 `IDF_PATH`，因此使用已有 clang-format
单独校验修改的 C/C++ 文件。

网络业务由本地 fixture 验证，真实网易云服务可用性与手机授权沿用已有实现。
本环境另一次 Open-Meteo HTTPS 请求在 TLS 握手阶段超时，因此本轮不声明真实天气网络连通性通过。
本轮未构建 MCU 目标或执行硬件测试。
