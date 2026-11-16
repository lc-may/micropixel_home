# D7：Host 时钟、持久闹钟与音频焦点

PC Home 默认使用四个原生 Wasm 槽，启动天气和时钟 Service，不预先创建页面。
音乐打开后加入同一 AppManager。`--single-app` 保留单槽路径；单独运行 Guest 的工具入口
仍可使用单槽。显式 `--multi-app` 保留组件演示所需的开发配置；默认 Home 的 task 应用
必须通过有限 Job/消息工作获得后台资格，单独声明 task 不会永久占用运行态。

```sh
bash tools/pc.sh build
bash tools/pc.sh run --home
```

run 会构建 `weather`、`clock-alarm` 和 `netease-music`，显示卡片、进入页面和后台播放共用
同一套组件实现。Home 默认目录只包含这三个内置应用及已安装应用，不扫描所有 conformance fixture。
D7 验证的是原生后端；RV32 多实例已在 [D8](app-manager-d8.zh-CN.md) 接入，MCU 组合留在 D9。

SDK 0.24.0 增加 Alarm 1.0（ID 28）、`Alarms` 和 `AudioFocusEvent`，七个 Core imports 不变。
`AlarmPlan` 可登记单次 UTC 时间或固定时区下的每周重复时间，星期掩码从周一 bit 0 到周日 bit 6。
Host 全局最多 32 条、每 App 最多八条。Clock Activity 提供时/分、每天/单次、保存启用、停用、
10 秒测试提醒、UTC/+8 时区切换及同步系统时间；Service 只发布 Widget，不参与响铃关键路径。

| 契约 | 行为 |
|---|---|
| 确认登记 | 参数、身份和容量检查后，SQLite FULL 同步提交成功才返回 OK |
| 持久记录 | AppId、88 字节已校验的线协议计划、最后 occurrence；数据库版本 2 兼容迁移 D6 记录 |
| 时间 | OS UTC 加显式校正、固定偏移 −720..840 分钟；无有效 UTC 不响铃 |
| 时钟变化 | 前后跳变、时区变更、首次恢复重新计算未来时间，不补响错过的提醒 |
| 重复去重 | 周期任务记录本地日期/分钟 occurrence，倒拨不重复当天已响事件 |
| 同时到期 | 一个 Host 弹窗合并多个提醒，铃声最多 60 秒，可手动停止 |
| 关闭 App | 保留已确认的 Host 闹钟，不重启被 user_stopped 的 Guest |
| 停用/删除 | 持久变更后取消相应闹钟；卸载/清除数据删除该 owner 的计划 |

Host 为时钟应用保留一个模板快照，即使 Guest 已停止或重启后受 user_stopped 阻止，Home 的
时间仍由 Host 更新。Widget 不嵌入 Guest Scene；闹钟开启状态来自持久计划。

响铃使用独立 Host 合成铃声，不占用 Guest PCM/synth 名额。音乐持续读取和播放，混音器将
普通音频降至 20%，停止铃声后只撤销衰减。Host 记录实例、媒体 owner、句柄、generation 和
intent_version；focus 不调用 Pause/Resume/Play，也不改变用户暂停宽限计时。
因此用户暂停、停止或切歌后不会被旧快照恢复。Service 可收到 focus 事件更新提示，物理声音
和铃声不依赖 Guest 回调成功。

Host 弹窗覆盖页面并取得输入优先级，开始时取消 Guest 当前触摸/文本焦点，结束后恢复前台
输入。弹窗提供停止响铃和暂停音乐，后者仍通过 MessageBroker 到音乐 Service。
系统主音量始终由 Host 管理。

卸载与清除数据先排空目标实例，再清理该 App 的 Job、导出权限、Alarm 和私有 KV。
卸载还移除 Host 安装目录内的 Bundle 并保存目录标记；开发目录的源文件/构建文件保留。
目录槽位不移动，避免改变其他 App 的身份，重新安装清除标记。

```sh
bash tools/pc.sh run --headless --clear-app-data micropixel.clock-alarm
bash tools/pc.sh run --headless --uninstall-app micropixel.clock-alarm

bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --alarm-store
bash tools/tests/test_firmware_host.sh --pc --alarms
bash tools/tests/test_firmware_host.sh --pc --alarms --soak
```

模型测试使用虚拟时间覆盖单次/每周、持久提交失败、重启、不补响、倒拨去重、时区与合并。
实际 Wasm 测试覆盖关闭 Clock 后响铃、重建 Host 后恢复未来计划、Host Widget 时间继续变化、
音频频率测量、用户暂停后保持静音、新媒体 generation 保留，以及卸载清理。
组合测试使用真实三 Guest、localhost 业务数据与合成 MP3，连续运行至少 10 分钟并刷新天气；
截图、PCM、日志仅保存在本地 build/。真实 Open-Meteo HTTPS 刷新在 D6 单独验证。

本阶段不包含 Guest 自动故障恢复、死循环指令预算、IANA/DST、贪睡或网络铃声。
checked AOT 与 Wasm 仍可使用；PC 三应用结果不代替 D8 的 RV32 或 D9 的目标板验证。
