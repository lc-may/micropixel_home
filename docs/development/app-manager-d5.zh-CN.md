# D5：Home Widget 与实例消息

SDK 0.22.0 增加 `Widgets`、`Messages` 和 `MessageEvent`，七个 Core imports 不变。
Widget 1.0（ID 25）和 Message 1.0（ID 26）通过原 Service ABI 协商；没有平台实现时能力为零。
音乐 0.6.0 在 PlaybackService 发布媒体快照并处理命令，Home 不读取账号、Cookie 或 Guest Scene。

| 线协议 | 固定长度 | 内容 |
|---|---:|---|
| widget_snapshot | 208 | weather/clock/media 模板、标题/副标题、更新时间、有效期、数值、播放进度 |
| message_send | 248 | 逻辑 AppId 或实例地址、组件代次、序号、期限、最多 128 字节载荷 |
| message_received | 240 | Host 注入的来源 AppId/实例/组件及复制的载荷 |
| message_ack/result | 12 | 票据、完成标记及执行结果 |

Host 固定容量为 64 个 App、每 App 两个 Widget、全局八个 Widget、32 条消息记录及
每 App 四条导出规则。快照字符串检查 UTF-8、长度与控制字符；只允许存活 Service 发布。
离线快照可保留并标记离线；容量满时只淘汰离线项。

Guest 发布序号单调递增。Host 的操作 revision 在曲目、状态、身份或组件代次变化时更新；
仅播放进度更新不使按钮失效。按钮提交整个期望快照，Host 校验 revision、实例与组件。
Home 在收到执行结果前禁用重复操作，不自动重试“下一首”。

同 App 组件和可信 Home 可以发送消息；跨 App 默认私有。Service 通过 `Messages::Allow()`
声明精确调用者与消息类型。导出权限随 Service 代次结束，不自动持久化。
消息只投给接受时解析出的存活接收实例，不向重启后的实例重放。
事件只携带票据，接收方 Read 后执行并 Ack，发送方 Result 查询结果。

相同 request_id 和相同内容返回同一票据；改写内容或换组件重用序号会被拒绝。
结果缓存有界，实例内序号水位拒绝已淘汰的旧命令。超时、停机和队列满分别返回明确状态；
停止接收后取消未完成消息，迟到 Ack 不改变取消结果。这不保证跨重启的恰好一次执行。

```sh
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --home-widgets
bash tools/tests/test_firmware_host.sh --pc --launcher
```

自动验证使用真实 Wasm 的三来源 Home、暂停/继续/下一首/刷新、页面状态保留、独立停止、
权限/去重/超时/代次拒绝以及坏线性地址的受控 Guest trap。截图与 PCM 只保存在本地 build/。
Job 冷启动与真实天气在 D6 接入；时钟闹钟与音频焦点在 D7 接入。
