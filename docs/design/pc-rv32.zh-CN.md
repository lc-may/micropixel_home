# PC RV32 AOT 后端

PC 保留 x86 WAMR portable Wasm 后端。RV32 后端用 QEMU `virt` 单核 M-mode 运行一个
ILP32F 小型 WAMR AOT 程序，执行官方 Bundle 中未经重新编译的 RISC-V AOT。
运行程序使用仓库固定 WAMR commit；不模拟 ESP32 外设，不改变 Guest ABI。

Linux Host 继续拥有 GuestContext、Service Registry、LVGL/SDL、音频和存储。
QEMU RAM 使用实例私有文件映射到 Host，UART 连接实例私有 Unix socket。
每次调用携带有版本的固定长度消息和当前线性内存基址、长度。Host 独立验证消息、
物理范围、线性范围、参数容量和服务句柄；Guest 偏移只能解析到当前线性内存。
消息和共享内存均不包含 x86 函数指针或 C++ 对象。运行程序等待回复期间不执行 Guest。

七个已有 Core imports 代理到 Host；`event_wait` 保持原事件队列语义，时钟来自 Host。
只有诊断帧携带最多 512 字节文本。线性内存最大 16 MiB，RAM 为 128 MiB。
不允许异步借用 Guest Direct Surface 缓冲区，Host-owned Surface、资源和 PCM 沿用原服务。

一个 AppSession 拥有一个 QEMU 子进程、一个服务线程及临时目录。停止时先请求 Guest
退出，超时终止并回收该子进程；服务释放后才解除 RAM 映射。所有错误回到大厅。
Bundle 使用原 reader 校验元数据、TOC、资源及 AOT 哈希，拒绝 Xtensa、不受支持的 AOT
格式和超限模块。市场下载还需校验所选 release 的 SHA-256 和大小。

验证覆盖官方包实际启动和可见输出、RV32 浮点调用、输入、音频、停止和重复启动，
以及截断消息、非法指针、错误架构和坏 Bundle 拒绝路径。测试统一从 Host 测试入口运行。

## 多实例与混合后端

PC Home 最多四个槽。原生解释器与 RV32 AOT 共用 AppManager、SessionRunner 和组件状态机，
各自保留 GuestContext、HTTP 两条通道、媒体实例、音频 owner、图形、输入和包租约。
每个 RV32 槽独立创建 QEMU、0700 临时目录、0600 RAM 文件和 Unix socket，子进程只继承标准
输入输出。日志附带 AppId、InstanceKey 和 PID；重用槽位时 epoch 递增。停止或替换包须等当前
worker 退出、QEMU 回收、RAM 解除映射后才释放槽位和预算，不向旧实例路由新事件。

AppManager 在入队和执行启动前两次检查空槽、内存与进程额度，拒绝启动时保留旧前台。
PC 预算为 576 MiB 和最多四个 QEMU；每槽预留 VM 上限（RV32 为 128 MiB，原生为 16 MiB）
加持有的模块及资源包字节。这是准入估算，未启动目录项的包、SDL/LVGL 缓冲、QEMU 程序和 OS 开销另计，
不是 RSS 保证。实际数量同时受四槽和上述预算约束。

原生启动期限为 10 秒，RV32 为 30 秒。RV32 完成 AOT 加载并调用 ABI 后才标记 VM 就绪；
组件声明应用还须注册生命周期，前台切换还须产生首帧。离线卡片的冷启动也使用相应后端期限，
启动完成后仍按原消息 TTL 处理业务。挂起前等待 Activity/Service 回调确认，
重复 EnsureService 不重复写入唤醒事件；管理器持续发布期望的挂起/恢复状态，避免错过过渡中的请求。
单个子进程退出只回收所属槽；这不包含自动重启和 MCU 故障恢复。

桥协议仍为 v1、固定 80 字节；七个 Core imports、Core ABI 2.0、AOT v6 及 WAMR 固定版本不变。
生命周期、Widget、Message、Job、Alarm 都通过既有服务通道传递。每个内存解析器只引用本槽映射，
拒绝越界、溢出和侵入启动区的内存窗口；无效窗口不会覆盖此前有效的范围。
官方旧包仅作为 LegacyActivity：Home 隐藏后挂起，返回原实例，不提供未经声明的 Service 或 Widget。

运行、能力矩阵与验证入口见 [D8](../development/app-manager-d8.zh-CN.md)。
