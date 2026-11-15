# 时钟与闹钟

Activity 设置单次/每天闹钟；Service 仅提供卡片。时间、持久计划、响铃、输入优先级和音频焦点
由 Host 执行，关闭 Guest 不取消已保存的闹钟。停用、卸载或清除数据可以取消计划。

```sh
bash tools/pc.sh guest guest/apps/clock-alarm
bash tools/pc.sh run --autostart build/pc-apps/clock-alarm/clock-alarm.pc.json
bash tools/pc.sh run --home
```

首次可点击“10 秒后提醒”验证声音。时区首版支持固定偏移，页面可切换 UTC/+8。
详细契约及自动测试见 [D7](../../../docs/development/app-manager-d7.zh-CN.md)。
