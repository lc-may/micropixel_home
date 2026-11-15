# PC Home 与应用可见性

`firmware/pc/home_app.*` 是 Host 常驻系统应用，负责本地应用目录、应用市场入口、当前应用和音频控制。
普通 Guest SDK 不提供启动其他 Guest 的接口。Home 通过 PC 组合根请求启动，Guest 不创建 Session。

PC Host 同时最多持有一个 Guest Session。Home 和市场显示于 LVGL 顶层，Guest 画面在下层保留；
系统页面可见时不向 Guest 转发触摸、按键或文本。图形引擎已有的 Host UI 检测阻止直接扫描输出覆盖系统页面。

| 操作 | 会话行为 |
| --- | --- |
| `Home` 或 `F1` 返回主页 | 保留 Guest；音频、HTTP、定时器和自动下一首继续运行 |
| Home 的“返回应用”、当前应用卡片或再次按 `Home/F1` | 复用原 Session，恢复输入，不重新登录或创建 Guest |
| Home 中打开应用市场 | 保留 Guest；市场返回 Home，下载与 Guest 请求分别有界执行 |
| 启动另一个 Guest | 先停止并释放旧 Session，再创建新 Session |
| `Esc`（Guest 在前台）或 Home 的“关闭应用” | 停止并释放 Session、音频及异步请求，返回 Home |
| Guest 正常退出、异常或关闭 Host | 统一清理 Session；不保留失效的当前应用入口 |

这里的后台是唯一 Guest 在 Host 系统页面后继续运行，不是同时运行多个 Wasm 应用。
音乐与另一个普通 Guest 同时运行需要进一步隔离图形、输入、网络、音频与设备租用，当前仍不提供。
设备固件的暂停/休眠会冻结音频，与 PC Home 的后台运行不同；本次只实现 PC 的组合与呈现策略。

Home 的运行态和导航由 Host 持有，不写入 Guest 私有 KV，也不读取音乐的 Cookie 或认证信息。
应用切换在主循环执行；不在 LVGL 回调或持有显示锁时等待 Guest 退出。
应用市场替换正在运行的同 ID 包时，先退出该 Session，避免包映射在执行过程中失效。

运行与验证见 [PC Host](../../firmware/pc/README.zh-CN.md)。

多个 Guest、桌面 Widget 与 AppManager 状态机的下一阶段方案见
[多应用与 AppManager 详细设计（HTML 提案）](app-manager.zh-CN.html)；该文档中的新接口尚未实现。
