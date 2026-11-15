# 天气

使用 Activity/Service、Job 和 Widget SDK 的最小天气应用。默认上海，可在页面切换北京、深圳。
每 15 分钟由 Host 调度刷新；关闭应用停止自动唤醒，Home 的刷新按钮可以重新启动。

数据来自 [Open-Meteo](https://open-meteo.com/)，使用 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)，
保持页面来源署名。API 返回的是气象模型当前值，天气代码采用 WMO 编码。

构建：`bash tools/pc.sh guest guest/apps/weather`。
运行：`bash tools/pc.sh run --multi-app --autostart build/pc-apps/weather/weather.pc.json`。
实现及测试见 [D6](../../../docs/development/app-manager-d6.zh-CN.md)。
