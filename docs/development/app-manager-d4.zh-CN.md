# D4：音乐 Activity 与 PlaybackService

音乐 Guest 0.5.0 使用同一套组件实现，不保留原页面入口。`main.cpp` 只组合 Application、
MusicModel、MusicActivity 和 PlaybackService。

| 部分 | 职责 |
|---|---|
| MusicModel | Account/Cookie、列表缓存、当前曲目、独立播放队列、页面状态及图片字节缓存；不保存 Activity 对象指针 |
| MusicActivity | Scene、两个 View、图片 Texture、文本/触摸、页面刷新 Timer；停止时不提交帧，销毁时释放页面资源 |
| PlaybackService | 独立 Timer、播放解析 Client、URL media、自动下一首；通过共享 Model 更新状态 |
| 页面请求 | 使用 Activity 资源域；隐藏时可由 Service 非阻塞轮询，页面销毁时关闭；新页面从 Model 恢复 |

Network **1.3** 保证两个有界执行通道：Activity/App 与 Service 各一个请求，两个独立 worker。
Service 通道不被二维码长轮询占用。旧 backend 默认只有一个通道并报告 1.2；新音乐明确要求 1.3。
Host Endpoint 保存两个实例内 lease，ComponentScope 在创建时决定通道，关闭/迟到响应按句柄代次处理。

媒体后台资格覆盖请求解析、下载、播放、用户暂停后 30 秒宽限，以及结束后的 1.5 秒换曲交接。
空闲音乐无有效资格时在 WaitEvent 安全点挂起。后台恢复不创建 Activity；显式打开才创建/恢复页面。
这使后续 Job 与消息租约可以接入相同判定。系统主音量继续由 Host 管理。

账号失效、退出或更换账号会取消播放请求并清空私有队列；Activity 正常重建保留账号与播放状态。
图片解码与帧提交只在 Activity 域执行，Service 保存可重建页面的数据。

```sh
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --playback-service
bash tools/tests/test_firmware_host.sh --pc --music
bash tools/tests/test_firmware_host.sh --pc --home
bash tools/tests/test_firmware_host.sh --pc --feed
bash tools/tests/test_firmware_host.sh --pc --launcher
bash tools/tests/test_firmware_host.sh --pc --login
```

关键自动断言：真实 35 秒页面长轮询未完成时，后台播放至少两首后续歌曲，另一个 Guest 前台接收按键；
销毁并重建已登录音乐页面后，Guest main、Service、登录及当前媒体均未重复启动。
既有匿名/完整/试听播放、账户隔离、每日推荐去重、滚动加载和单槽替换回归继续保留。
测试使用 loopback API 与合成 MP3，不需要真实账号。日志、PCM 与截图保存在本地 `build/`。

音乐 release Bundle 仍使用固定 wamrc、riscv32-ilp32f 和 checked memory；PC 多实例仅启用原生 Wasm。
Home Widget/消息在 D5 接入，Job 在 D6 接入，闹钟/焦点在 D7 接入。MCU 媒体与多实例仍属于 D9。
