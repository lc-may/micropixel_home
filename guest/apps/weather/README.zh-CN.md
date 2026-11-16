# 天气

使用 Activity/Service、Job 和 Widget SDK 的最小天气应用。默认上海，可在页面切换北京、深圳。
每 15 分钟由 Host 调度刷新；关闭应用停止自动唤醒，Home 的刷新按钮可以重新启动。
页面使用深灰卡片、大号温度和天气图形，显示湿度、风速、更新时间与刷新按钮。
缓存格式兼容旧温度记录；缺少湿度或风速时显示 `--`。Home 使用同一城市和最新成功值。

数据来自 [Open-Meteo](https://open-meteo.com/)，使用 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)，
保持页面来源署名。API 返回的是气象模型当前值，天气代码采用 WMO 编码。

构建：`bash tools/pc.sh guest guest/apps/weather`。
运行：`bash tools/pc.sh run --multi-app --autostart build/pc-apps/weather/weather.pc.json`。
实现及测试见 [D6](../../../docs/development/app-manager-d6.zh-CN.md)。
新版界面见[面板指南](../../../docs/development/panel-ui.zh-CN.md)。
