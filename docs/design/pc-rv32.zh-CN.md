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
