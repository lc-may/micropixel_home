# PC Home 与应用可见性

`firmware/pc/home_app.*` 是 Host 常驻系统应用，负责本地应用目录、应用市场入口、当前应用和音频控制。
普通 Guest SDK 不提供启动其他 Guest 的接口。Home 通过 PC 组合根请求启动，Guest 不创建 Session。

PC Home 默认最多四个 Guest Session，支持原生 Wasm、RV32 和混合后端。`--single-app` 保留单槽兼容配置。
Home 和市场显示于 LVGL 顶层，Guest 画面在下层保留；
系统页面可见时不向 Guest 转发触摸、按键或文本。图形引擎已有的 Host UI 检测阻止直接扫描输出覆盖系统页面。

| 操作 | 会话行为 |
| --- | --- |
| `Esc`、`Home` 或 `F1` 从 Guest 返回主页 | 保留 Guest；音频、HTTP、定时器和自动下一首继续运行 |
| 应用目录中的“返回应用”、音乐卡片或再次按 `Home/F1` | 复用原 Session，恢复输入，不重新登录或创建 Guest |
| Home 中打开应用市场 | 保留 Guest；市场返回 Home，下载与 Guest 请求分别有界执行 |
| 启动另一个 Guest | 有空槽和预算时启动并切换前台，原 Service 按资格继续工作；单槽配置先释放旧 Session |
| 应用目录的“关闭当前应用” | 停止最近的当前应用并释放其 Session、音频及异步请求；其他应用继续运行 |
| Home 中按 `Esc` | 保持主页，不停止或恢复 Guest |
| 市场或闹钟弹窗中按 `Esc` | 市场返回上一级，闹钟停止响铃；不停止后台 Guest |
| Guest 正常退出、异常或关闭 Host | 统一清理 Session；不保留失效的当前应用入口 |

各槽拥有独立资源和 Activity/Service 生命周期。v1 应用隐藏后挂起，v2 Service 按媒体租约或
Job/消息资格继续工作。显式 `--multi-app` 额外开放组件演示的 task 开发配置。
设备固件的暂停/休眠会冻结音频，与 PC Home 的后台运行不同；本次只实现 PC 的组合与呈现策略。

Home 的运行态和导航由 Host 持有，不写入 Guest 私有 KV，也不读取音乐的 Cookie 或认证信息。
Home 显示实时时钟、天气、下一次闹钟、待提醒事项与音乐播控，界面布局见[家庭面板](panel-ui.zh-CN.md)。
“应用”打开目录与市场入口，闹钟和日历使用 Clock Widget 的可选 `OpenPage` 消息进入指定页面。
应用切换在主循环执行；不在 LVGL 回调或持有显示锁时等待 Guest 退出。
应用市场替换正在运行的同 ID 包时，先退出该 Session，避免包映射在执行过程中失效。

运行与验证见 [PC Host](../../firmware/pc/README.zh-CN.md)。

组件契约见[组件与资源](app-components.zh-CN.md)，多 RV32 运行见 [D8](../development/app-manager-d8.zh-CN.md)；设计背景见
[多应用与 AppManager 详细设计（HTML 提案）](app-manager.zh-CN.html)。
