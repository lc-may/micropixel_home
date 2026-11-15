# D6：天气与后台工作

SDK 0.23.0 提供 Job 1.0（ID 27）、`Jobs`、`JobEvent` 和 `WallTime`。
Host 保存 AppId 所有的周期/单次计划；工作票据与普通消息只在本次启动有效。
七个 Core imports、WAMR/wamrc 固定版本和 checked AOT 不变。

| 容量/规则 | 值 |
|---|---|
| Job 计划 | 全局 32，每 App ID 1–2 |
| 周期 | 60–86400 秒；天气默认 900 秒 |
| 工作 deadline | 单调时钟 1–60 秒；天气默认 20 秒 |
| 消息冷唤醒预算 | 全局八条，沿用消息 100–60000 ms 期限 |
| 冷唤醒导出规则 | 每 App 四条精确调用者/消息类型规则 |
| Home 离线操作 | 八条、10 秒准备期限，接收端 Ready 后生成实例消息 |

AppManager 的 `EnsureWork` 复用 RUNNING、恢复 SUSPENDED，或在 STOPPED 时启动仅含
Service 的实例，不创建 Activity、不修改前台。显式 Activate 才创建/恢复 Activity。
采用 Job 的应用不因声明 task 就永久运行：前台、媒体、活动 Job、待处理消息/Widget 操作提供资格，
空闲后在 WaitEvent 安全点挂起。Guest Timer 随实例冻结，Host Job 时限继续推进。

Service 在处理 Job 时先 `Enter(ticket)`，后续 Service HTTP 标记该工作。
Host deadline 取消匹配票据的 I/O，并通知 Service 清理和 Complete。
不同工作独立完成，旧票据不能关闭新请求。天气顺序处理最多两个工作，保留最后成功值；
网络错误、坏 JSON、超时或刷新期间城市变化都不会把旧城市的结果写到新城市。

Job 计划、user_stopped 和显式导出权限保存在 `schedules.sqlite3`，格式版本 1，SQLite FULL
同步提交后才确认登记。正常重启恢复未来计划；工作票据不恢复。按 AppId 清理计划也删除导出规则。
用户 Stop 阻止自动 Job/消息拉起；打开应用或点击离线天气卡片“刷新”清除标记。
卡片刷新启动新的 Service，随后向新实例投递一次命令；旧实例消息永不改投。

跨 App 冷启动默认禁止。冷唤醒要求 Message 1.1（requirements 中 message 65537）；普通实例消息仍支持 1.0。接收 Service 通过 `Messages::AllowColdStart(caller, types)` 显式授权并持久提交。
允许的逻辑发送在准备期间返回 WouldBlock；调用者用**同一 request_id 和内容**在原期限内重试。
只有实例 Ready、消息真正入队后才返回投递票据。实例地址不触发冷启动，也不提供跨重启恰好一次。
user_stopped、来源实例退出、配额与期限均由 Host 再检查。

天气 Guest 位于 [guest/apps/weather](../../guest/apps/weather/main.cpp)，Activity 展示详情、切换
上海/北京/深圳与手动刷新，Service 使用 SDK HTTP/HTTPS 直连
[Open-Meteo Forecast API](https://open-meteo.com/en/docs)，无网关、无仓库密钥。
采用 current temperature_2m/weather_code、UTC Unix 时间，页面显示来源及 CC BY 4.0 署名。
城市和最后成功值使用 AppId 私有 KV；测试来源通过启动参数注入，不修改正式来源。

```sh
bash tools/pc.sh guest guest/apps/weather
bash tools/pc.sh run --multi-app --autostart build/pc-apps/weather/weather.pc.json
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --jobs
```

自动检查覆盖虚拟时间周期/单次计划、持久提交失败、独立票据与代次、实际 Wasm 的冷/热/挂起工作、
HTTP 成功/失败/慢响应、旧值保留、用户 Stop、显式 Widget 冷刷新和正常重载/清理。
真实来源验证通过 PC Host/Wasm 发起 HTTPS 请求；日志仅保存在本地 build/。
MCU 后台组合与 RV32 多实例仍分别属于 D9/D8。
