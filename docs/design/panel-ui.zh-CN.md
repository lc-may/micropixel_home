# 家庭面板界面

参考 [ORVIBO MixPad 4 官网](https://www.orvibo.com/cn/product/mixpad_m2.html)的产品屏幕，
采用深色背景、略透明的灰色卡片、白色文字与橙色主控按钮。Home 参考主视觉的圆形模拟时钟，
音乐页参考顶部专辑信息、中央圆形播放键和横排播控；天气页使用大号温度、天气图形及少量指标。

参考图片仅用于设计对照，运行界面由 LVGL/Guest Scene 的原生图形绘制。源码不包含官网产品图片。
可对照官网的[主视觉](https://www.orvibo.com/guanwang_second/front/images/products/mixpad_m2/banner-9316ca99ba.jpg)及
[全屏面板](https://www.orvibo.com/guanwang_second/front/images/products/mixpad_m2/sec6-31151f66fd.jpg)。

官网功能说明强调触屏控制、背景音乐与简洁的全屏界面。视觉提取如下：

| 官网画面特征 | MicroPixel 对应设计 |
|---|---|
| 米白模拟表盘、深色刻度、红色秒针 | Home 的实时时钟，旁边显示日期与天气 |
| 黑灰底色与低对比卡片 | 四个页面统一深灰表面、白字及灰色辅助文字 |
| 琥珀色主开关、音乐圆形播放键 | 选中页签、保存/刷新按钮与中央播放键使用橙色 |
| 专辑/曲名在上，切歌与进度在下 | 音乐页保留顶部封面信息、横排播控、收藏与进度条 |

日历、闹钟和 Note 是本项目按需求提供的应用功能，沿用同一配色和触控尺寸。
面板使用 720×720 逻辑坐标；Home 在 Host 中绘制，三个应用仍通过 Guest Scene SDK 绘制。
主色为 `#15181D` 背景、`#272B32` 卡片、`#F5F6F8` 正文和 `#F5AE39` 橙色强调。

| 页面 | 功能 |
|---|---|
| Home | 模拟表盘、日期、城市天气、下一次闹钟、待提醒事项、音乐播控；底部进入日历/天气/音乐/应用目录 |
| 音乐 | 网易云官网微信登录、个人歌单、推荐、搜索、红星收藏；专辑信息、播放/暂停、切歌、进度拖动、返回列表 |
| 天气 | 上海/北京/深圳、当前天气与温度、湿度/风速/更新时间、刷新；后台缓存与任务策略沿用天气 Service |
| 日历与提醒 | 闹钟、月历、事项三页；选日期添加提醒、编辑名称和时间、完成/恢复、删除 |

天气温湿度与风速来自所选城市的 Open-Meteo 气象数据，服务端缺少的指标显示 `--`。
使用[公开 Forecast API](https://open-meteo.com/en/docs)的 current 字段，保持来源标注。

事项使用同一 Clock Guest 的 Alarm 计划 3–8，最多六条。标题与提醒时刻保存在 Host 持久计划中，
手动完成标记保存在 App 私有 KV。闹钟保留计划 1，临时测试提醒使用计划 2。
完成事项会取消对应提醒，恢复未来事项会重新启用；到期提醒不会自动等同于手动完成。
删除事项会删除 Host 计划。关闭 Activity、关闭应用或重启 Host 后，已确认的未来提醒仍生效。
单次提醒错过后不补响，遵循现有 Alarm 契约。

Home 的日程摘要读取 Host 已确认的计划，时间和响铃不依赖 Clock Guest 常驻运行。
Clock Widget 支持可选 `OpenPage` 消息（type=4，U32 0=闹钟、1=日历、2=事项）；
沿用现有 Message 通道、身份校验、票据和冷启动规则，七个 Core imports 及线协议布局不变。

Guest 统一使用 `sdk/ui/panel.hpp` 配色/卡片/数字绘制，以及 `sdk/calendar.hpp` 的日期计算。
日历辅助函数支持公历日期、星期、闰年、月份长度和固定时区转换。
Note 标题支持 PC 键盘和输入法，最大 63 UTF-8 字节；输入后按回车收起，点击保存提醒。

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home
# 同一界面的 RV32 checked AOT 版本。
bash tools/pc.sh run --home --rv32
```

PC 指定 `--screenshot 文件.png` 后，F12 可随时保存当前屏幕，退出时仍保存最终画面。
Home 的“应用”打开目录和市场入口；Home/F1 返回面板，音乐继续播放。Esc 关闭当前应用。
