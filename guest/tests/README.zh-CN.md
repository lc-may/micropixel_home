# Guest conformance tests

这里保留仍由 `tools/build_guest_p4.sh` 构建的 ABI/SDK 回归用例，包括成功路径、非法 import/指针、
watchdog、退出语义、Service、Event、Graphics、Input、Audio 和 no-WASI STL 边界。

```sh
bash tools/build_guest_p4.sh
```

生成的 Wasm/AOT 位于 `build/guest-p4/`，不进入版本控制。`event_wait`、`touch_pressure` 和
`run_handler_*` 在真机运行时需要 Host 的合成事件钩子；使用
`firmware/espressif/sdkconfig.p4-conformance.defaults` 构建专用固件。
其余产品构建继续使用 `sdkconfig.p4.defaults`，不会链接这些钩子。

`linear_memory_limit` 是产品策略真机回归：默认 Host 必须拒绝 9 MiB 的 Guest 动态分配，同时允许
7 MiB 分配并访问首尾字节。它验证的是当前 8 MiB linear-memory 上限，因此不作为脱离 Host 的 Wasm
执行用例。

新增用例必须有明确断言并进入上述构建入口；仅用于一次测量的实验程序和原始数据不放在这里。

公共 UI 组件的真实 Wasm/SDL 交互回归使用 PC 入口：

```sh
bash tools/tests/test_firmware_host.sh --pc --ui-components
bash tools/tests/test_firmware_host.sh --pc --panel
```

`ui_components.cpp` 检查子树移动/析构/重复销毁、失败布局回收、文本长度、固定池容量、
追加保留偏移、内容状态与绑定缓存，以及节点槽复用后的 Scene 文本压缩。`test_pc_ui_components.py` 在目标 X11 窗口注入触摸和按键，
覆盖平移父容器、正文/尾部动作隔离、按住时更换数据、边界拖动取消点击、禁用控件与节点数量不增长。
截图和日志保存在忽略的 `build/pc-ui-component-tests/`；测试使用独立数据目录，不需要真实账号。
