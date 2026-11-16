# 天气

使用 Activity/Service、Job 和 Widget SDK 的最小天气应用。默认上海，内置全国 34 个省区的 394 个城市/地区。
选择页首屏是 10 个常用城市；也可按省份选择，列表支持滚动，重启后保留选择。
每 15 分钟由 Host 调度刷新；关闭应用停止自动唤醒，Home 的刷新按钮可以重新启动。
界面依据 [720×720 设计稿](../../../docs/ui_design/index.html)：126 px 主温度、5 小时与未来 3 日预报，底部为湿度和风速。
点击左上城市切换，右上刷新；Material 圆润图标区分晴、多云、阴、雾、雨、雪和雷暴。
版本 4 重置旧三城列表设置；缺少预报或指标时显示 `--`，失败保留最后成功值。Home 使用同一城市和最新成功值。
离线位置数据与许可见[城市数据](data/README.md)。

数据来自 [Open-Meteo](https://open-meteo.com/)，使用 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)，
保持页面来源署名。API 返回的是气象模型当前值，天气代码采用 WMO 编码。

构建：`bash tools/pc.sh guest guest/apps/weather`。
运行：`bash tools/pc.sh run --multi-app --autostart build/pc-apps/weather/weather.pc.json`。
实现及测试见 [D6](../../../docs/development/app-manager-d6.zh-CN.md)。
新版界面见[面板指南](../../../docs/development/panel-ui.zh-CN.md)。
