# 日历与提醒

Activity 包含闹钟、设置、月历和事项页面，依据 [720×720 设计稿](../../../docs/ui_design/index.html)使用深色卡片、琥珀色开关、96 px 数字和 Material 图标。
闹钟页显示主闹钟和真实事项摘要；点击时间卡或右上加号设置闹钟。底部按钮进入日历、事项。
月历支持切换月份、回到今天、选日添加事项；事项支持名称/日期/时间编辑、完成/恢复、删除。
点击名称框后使用 PC 键盘或输入法，回车收起，再点击保存。最多六条，名称最大 63 UTF-8 字节。

Service 提供卡片与页面导航。时间、持久计划、响铃、输入优先级和音频焦点
由 Host 执行，关闭 Guest 不取消已保存的闹钟。停用、卸载或清除数据可以取消计划。

```sh
bash tools/pc.sh guest guest/apps/clock-alarm
bash tools/pc.sh run --autostart build/pc-apps/clock-alarm/clock-alarm.pc.json
bash tools/pc.sh run --home
```

进入闹钟设置后可点击“10 秒后提醒”验证声音。时区首版支持固定偏移，页面可切换 UTC/+8。
详细契约及自动测试见 [D7](../../../docs/development/app-manager-d7.zh-CN.md)。

主闹钟使用计划 1，10 秒测试使用计划 2，事项使用计划 3–8。
完成事项会停用提醒；恢复未来事项会重新启用。已到期的事项显示“已提醒”，可手动完成或删除。
事项数据保存在 Host 闹钟计划，完成标记保存在 App 私有 KV；关闭页面和重启后保留。
`OpenPage` 可选消息用 U32 指定页面：0=闹钟，1=日历，2=事项。
Home 的闹钟卡片直接进入闹钟页，“日历与事项”卡片进入月历页。
界面及验证见[面板指南](../../../docs/development/panel-ui.zh-CN.md)。

`main.cpp` 只组装应用；`planner.hpp` 维护模型，`clock_service.hpp` 处理 Home 数据和页面消息，
`clock_activity.hpp` 管理生命周期与公共 AppBar，`clock_page.hpp` 管理当前页面子树。
页面通过 SDK ComponentRoot 释放节点，空事项提示使用 ContentStateView；停止页面时取消输入与文本编辑。
显示配置在 main 中只执行一次，页面重建只重建 Scene 与控件。
