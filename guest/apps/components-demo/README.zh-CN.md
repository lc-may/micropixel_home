# Activity + Service 示例

`Model` 保存共享业务状态，`DemoActivity` 持有界面和输入，`DemoService` 用 Timer 驱动 PCM 输出。
Service 不保存 Activity 指针；销毁页面后 AppModel 与 Service 继续存在。

```sh
bash tools/pc.sh guest guest/apps/components-demo
bash tools/pc.sh run --multi-app --autostart build/pc-apps/components-demo/components-demo.pc.json
```

按 Home/F1 返回系统主页。页面按钮分别销毁 Activity、启动/停止 Service、退出整个 App。
从 Home 再打开同一应用会重建 Activity，不重启 Guest main。

应用使用 `Application::RunComponents(&activity, &service)` 取代 `Application::Run(handler)`。
回调的 ComponentContext 自动选择资源域；通过 `context.WithScope(...)` 显式使用保存的域。
ConfigureDisplay 是 App 初始化操作，只调用一次；每次 OnCreate 重建 Scene。

示例故意把部分资源留到 Host 域清理，以验证隔离；业务应用仍应在 OnDestroy / OnStop 释放 RAII 对象。
协议、状态和容量见[组件契约](../../../docs/design/app-components.zh-CN.md)。
