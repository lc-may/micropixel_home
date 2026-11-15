# PC AppManager 与组件契约

组件实现使用原生 WAMR 解释器。PC Home 默认四槽，`--single-app` 保留兼容路径，`--multi-app` 显式启用开发配置。
MCU 组合根尚未接入多会话；RV32 多实例在 D8 接入。七个 Core imports、Core ABI 2.0、AOT v6 和
Bundle v1 外壳保持原样。后续阶段见[交付计划](../development/app-manager-plan.zh-CN.html)。

## 管理与可见性

`AppManager` 是单写者控制器，最多 64 个目录项、4 个实例、32 条待执行命令。Guest 不直接调用它。
Host Home 发起 Activate / ShowHome / Stop，控制器通过 `SessionRunner` 发起异步操作，并消费带
`InstanceKey{boot, slot, epoch}` 的观察结果。槽位复用增加 epoch，过期观察不能修改当前实例。
同一 AppId 只保留一个目录项与一个实例。目录包在运行期间保持有效，替换先停止对应实例。

App 状态为 `Stopped → Starting → Running ↔ Suspended → Stopping → Stopped`。
Activate 复用已有实例；新实例准备就绪且发布首帧后才交接前台。启动准备超过 10 秒请求停止；
旧前台保留。容量耗尽返回 NoCapacity，不自动结束其他应用。停止由会话控制线程推进，UI 不等待
Guest safe point。退出后先排空本实例任务再归还槽位；这属于正常退出协议，不提供任意死循环的有界终止保证。

多槽下 v1 应用为 LegacyActivity：隐藏后在 WaitEvent 安全点挂起，恢复同一实例。
v2 的后台资格来自 Host 解析的组件声明。D3 开发配置允许声明后台 task / media 的 Service 常驻；
JobScheduler 提供有限任务资格；默认 Home 使用 MEDIA / JOB / 待处理消息决定后台运行。单槽下旧音乐在 Home 后继续运行，切换另一应用才结束它。

## Activity 与 Service

每个 Guest 有一个 Application、最多一个 Activity 和一个 AppService，共享线性内存和 AppModel。
`Application::RunComponents()` 是唯一事件循环，生命周期和业务回调串行执行。Service 不另建 Guest 线程。

| 组件 | 状态和回调 |
|---|---|
| Activity | None → Created (`OnCreate`) → Resumed (`OnResume`) → Stopped (`OnStop`)；销毁调用 `OnDestroy` 后回到 None |
| Service | Stopped → Starting (`OnStart`) → Running → Stopping (`OnStop`) → Stopped |

隐藏 Activity 保留页面资源；DestroyActivity 销毁其资源域，Service 继续。CreateActivity 分配新组件代次，
Guest main 不重启。StopService 关闭 Service 域，前台 Activity 保留。Exit 或 Host Stop 结束整个 App。
单次工作通过 Job work ticket Complete 结束；StopService 结束整个 Service 代次并取消其未完成工作。

组件 token 是实例内能力编号，低两位表示种类，高位表示代次。相同数字可在不同实例出现，不能作为跨实例地址。
Host 在当前 GuestContext 中校验 token、transition sequence 和 ACK command；不接受重复或过期 ACK。
资源句柄只在原 Guest 的 service handle / context 内解析。

## 生命周期 Service 与 requirements

Lifecycle Service ID 为 **24**，接口 **1.0**，使用原有 service_call 与 48-byte Event。

| Method | ID | 请求 / 响应 |
|---|---|---|
| Register | 1 | 注册声明和 Service 初始启动意图；8 / 24 bytes |
| Ack | 2 | token、transition、command；16 bytes |
| Scope | 3 | 选择组件资源域，0 为 App 共享域；8 bytes |
| Request | 4 | 创建/销毁 Activity、启动/停止 Service；kind=0/Stop 结束 App；8 bytes |
| Route | 5 | service ID + 原资源 handle → 原组件 token；12 / 8 bytes |
| Query | 6 | 声明、状态、token、资源记录数；0 / 24 bytes |

生命周期 Event ID 1 的 16-byte payload 为 token、command、transition、reserved0。
size 必须精确匹配，reserved 为零。短响应返回 BufferTooSmall 且不注册组件；未声明组件、非法操作和过期代次被拒绝。
SDK 负责 wire 转换；应用使用[组件 SDK](../../guest/sdk/components.hpp)。

```json
"requirements": {
  "schema_version": 2,
  "display": {"layouts": ["square"], "min_width": 240, "min_height": 240},
  "required": [], "optional": [], "any_of": [],
  "services": {"lifecycle": 65536},
  "components": {"activity": true, "service": true, "background": ["task"], "widgets": 0}
}
```

至少声明一种组件；background 仅允许不重复的 media / task，且要求 Service。组件使用单线程 Guest。
widgets 解析预留值 0–2，D3 Host 只接受 0。v1 禁止 components 字段；旧 Host 不支持 schema 2，明确拒绝新包。
当前 MCU 环境的 Lifecycle 版本为 0，因此不会误接纳新组件包。PC 描述符和 Bundle 共用 Host requirements reader。
Bundle metadata 中还必须带 `core_abi: 131072`，CLI 自动写入。

Registry 固定容量从 16 扩至 24，目前使用 15 个端点；新增存储为
`8 × (sizeof(ServiceHandler*) + sizeof(ServiceDescriptor))`，64-bit PC 为 384 bytes。
组件记录另占 `128 × 20 = 2560 bytes` 加状态机/锁；MCU 对齐与总上下文内存在 D9 构建时核算。Widget 25、Message 26、Job 27、Alarm 28 已接入，固定目录使用 19/24。服务目录数量从名称表推导，避免扩展遗漏空项。
它们在 D3 不注册、不宣称能力，不提前占用公开数字 ID。

## 资源归属和上限

物理 SDL display、SDL mixer 和只读设备目录由 Host 常驻；可变资源通过每槽独立设备视图访问。
共享桌面字体的 TinyTTF cache 回调使用 LVGL 适配锁，防止不同 Guest 同时修改缓存。

| 资源 | 归属与容量 | 回收 |
|---|---|---|
| WAMR / 任务 | 每实例一模块、执行线程和控制线程；线性内存最多 256 pages（16 MiB），模块较小上限优先 | Guest 结束、Context 关闭、执行环境释放、本 owner 的 FreeRTOS 兼容任务 join |
| Scene / Raster / Surface | 每实例独立引擎、保留树、纹理访问和显示根；每槽一个 App Surface；Scene 提交最多 128 KiB | 隐藏保持，Activity 销毁释放其显示树；实例停止释放所有资源 |
| 纹理 / 字体 | 每实例纹理最多 128、动态字体 8；组件记录总表 128 | 按创建域清理；Activity 清理保留 App/Service 字体 |
| Timer / Event | 每实例 Timer 8、EventQueue 16，加既有周期事件合并槽 | 销毁组件关闭原域 Timer；已退休句柄的排队事件不再路由 |
| PCM / 合成音 | 每实例 PCM ring 1（256–65536 frames）；物理共享 PCM voice 4、合成 voice 8 | sink、暂停、StopAll 按 owner；组件 StopAll 仅关闭当前域；不抢占其他 owner 的活动 voice |
| HTTP / URL media | 每槽独立请求与媒体 worker，各一个活动 lease；HTTP body 最大值由 ABI 限制 | close 取消并提高 generation，迟到结果不覆盖新请求；Service 请求不随页面销毁 |
| 媒体解码 | PC 下载输入最多 32 MiB，解码最多 64M 个 int16 samples | 停止解除音频借用；槽位 worker 可复用，Host 退出时排空；MCU 有界流解码属于 D9 |
| 输入 / 存储 | 每槽独立触摸、按键、文本绑定；KV 按 AppId 隔离，PC 默认 64 KiB / 128 keys | 失焦发送 Cancel 并清空文本，物理输入仅转发前台；账号 KV 跨实例重建保留 |

128 条组件记录覆盖 Timer、纹理/字体、clip/playback/PCM/media、HTTP 和 Surface。资源创建前检查容量，
记录创建时的 token；异步任务保留该 token，不读取完成时的可变 current scope。Scope 0 的共享资源在 Activity
销毁后继续存在。OnDestroy / OnStop 应释放自己的 RAII 对象，Host 域清理提供正常遗漏时的兜底；已失效对象的 Reset 安全忽略关闭失败。

Host Stop 的顺序是停止输入和新工作、唤醒 Guest 协作退出、关闭请求/媒体/PCM、排空显示引用、关闭
Context、销毁 WAMR，再 join 所属任务。槽位图形缓冲和 worker 是固定上界缓存，不以 RSS 完全回落作为逐次回收的定义。

本阶段不增加 crash 自动恢复、AOT 检查点、指令预算或原生内存故障隔离。WAMR 的现有内存检查保持启用。
PC 的测试结果不能作为 RTOS/no-MMU 已通过的证据。

## 最小示例与验证入口

[components-demo](../../guest/apps/components-demo/) 演示共享 AppModel、Activity 重建、Service PCM 和定时器。
[阶段验证记录](../development/app-manager-d1-d3.zh-CN.md)列出 wrapper 和覆盖范围。

后台任务与界面交互见 [D6](../development/app-manager-d6.zh-CN.md) 和 [D7](../development/app-manager-d7.zh-CN.md)。
