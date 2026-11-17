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
bash tools/tests/test_firmware_host.sh --pc --ui-components
bash tools/tests/test_firmware_host.sh --pc --panel
bash tools/tests/test_firmware_host.sh --pc --panel --rv32
```

Guest 公共组件首版的回归入口与覆盖如下。本轮这些 PC 测试均通过，使用本地 fixture 和独立数据目录。

| 命令（接在 `bash tools/tests/test_firmware_host.sh` 后） | 覆盖 |
|---|---|
| `--pc --ui-components` | 组件移动/清理、失败布局回收、文本压缩、稳定 key、按住重绑取消、边界拖动、禁用与固定节点池 |
| `--pc --panel` | 三个 App 的实际页面、城市与天气缓存、事项持久化、音乐交互及后台 PCM |
| `--pc --widgets` | 原有 Slider、ScrollView、ListItem 的手势与 pending 状态兼容 |
| `--pc --music` | SDK/wire 边界、搜索与播放状态、协议编解码和非静音 MP3 |
| `--pc --feed --refresh` | 推荐刷新保留内容、切页、部分追加与去重 |
| `--pc --playback-service` | 长轮询、后台自动下一首、页面销毁后重建与账号/队列保留 |

三个应用的 checked RV32 release Bundle 输出在 `build/guest-ui-release/<app>/`，
均通过 PC/QEMU RV32 的创建页面与正常退出检查。该检查覆盖启动，不代表 RV32 全量交互回归或 MCU 真机验证。
组件契约见[独立 HTML 设计](../design/guest-ui-components.zh-CN.html)，API 与调用要求见
[SDK](../../guest/sdk/README.zh-CN.md#组合控件)。

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
模型、公历对照、天气解析与 33 个 Material 图形展开测试通过。本轮以下相关回归通过：

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

## 新闻与紧凑播控

Home 音乐控件的可见背景缩为 168×60 圆角矩形，主按钮为 64×48，两个操作仍各有 84×84 触摸区。
以下测试通过：

```sh
bash tools/tests/test_firmware_host.sh --pc --news
bash tools/tests/test_firmware_host.sh --pc --news --rv32
bash tools/tests/test_firmware_host.sh --pc --news --live
bash tools/tests/test_firmware_host.sh --pc --news --rv32 --live
```

测试使用真实 SDL 窗口和 130 KiB fixture，检查 50 条新闻、加载骨架、滚动不误选、完整标题与热度、
已读/未读、空列表、503 保留缓存、隐藏时取消请求、关闭/重开后恢复缓存与已读标识。
同时运行天气、时钟、音乐和新闻，验证浏览/关闭新闻时 PCM 持续输出；Home 新播控的可见面积、背景颜色、暂停和恢复也被检查。
`--live` 通过 Guest Network SDK 直连公开今日头条 HTTPS 接口，实际解析 50 条内容。
截图位于 `build/news-tests/`、`build/news-rv32-tests/`，运行与接口边界见[新闻应用](../../guest/apps/news/README.zh-CN.md)。

## 新闻正文

新闻现默认使用央视新闻首页的 JSONP 列表，点击卡片后通过文章 HTTPS 地址下载真实正文。
顶栏左侧切换央视新闻和头条话题；头条话题不冒充文章正文。正文保留段落与来源，支持滚动、重试、取消后恢复，
并缓存最近一篇文章的最多 3072 个 UTF-8 字节；长文离线内容明确标为节选。

```sh
bash tools/tests/test_firmware_host.sh --pc --news --content
bash tools/tests/test_firmware_host.sh --pc --news --content --rv32
bash tools/tests/test_firmware_host.sh --pc --news --content --live
bash tools/tests/test_firmware_host.sh --pc --news --content --rv32 --live
```

原生 Wasm 和 RV32 AOT 的正文下载、实际显示、滚动、来源切换、错误/视频处理、重试、离线节选、
隐藏/恢复和关闭重开测试通过，四个应用共存时 PCM 持续输出。两个后端的真实央视列表及正文 HTTPS 验证通过。
模型测试验证 JSONP、文章路径、HTML 正文区域与引用字符串、实体、UTF-8 长文分行、视频代码去除和响应长度边界。
原有头条热榜交互回归也通过。
截图位于 `build/news-content-tests/` 和 `build/news-content-rv32-tests/`，
接口地址与资源边界见[新闻应用](../../guest/apps/news/README.zh-CN.md)。
