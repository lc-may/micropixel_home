# 网易云音乐

MicroPixel SDK 应用：单线程 Wasm、Scene/TextButton 界面、HTTP 搜索、URL 音频播放。
直接连接网易云，匿名模式可播放接口允许的音乐；会员、登录及地区限制会显示错误或试听状态。

登录成功或恢复会话后默认进入“我的音乐”，显示昵称、签名、头像及创建/收藏的歌单。
底部切换“每日推荐 / 我的音乐 / 搜索”，列表支持滑动。点击歌曲进入独立播放页，
支持封面、红星、上一首/下一首、播放/暂停、停止和拖动进度；松手才提交跳转。
新版采用 MixPad 4 面板风格：深灰/暖棕背景、顶部专辑信息、中央橙色圆形播放键、横排切歌和底部进度条。
列表、账户与登录页面使用相同配色，收藏保持红星；列表按钮返回当前音乐来源。
底部迷你播放器保持当前播放，切换页面不会替换播放队列。
红星同步加入或移出网易云“我喜欢的音乐”；等待服务器确认时阻止重复提交，失败后重新同步状态。
“账户”管理记住登录和退出；会话失效或退出时清除个人列表、头像和播放队列。

在仓库根目录执行：

```sh
bash tools/pc.sh build
bash tools/pc.sh run --music
# 直接打开微信扫码登录界面
bash tools/pc.sh run --music-login
# 指定搜索词，或直接播放歌曲 ID
bash tools/pc.sh run --music-query '纯音乐'
bash tools/pc.sh run --music-song 139774
```

`run --music` 会先增量构建 Guest。也可以单独构建后从应用大厅启动：

```sh
bash tools/pc.sh guest guest/apps/netease-music
bash tools/pc.sh run
```

点搜索框后用键盘输入，回车搜索；Backspace 删除一个 UTF-8 字符。
列表滚动到底会自动追加内容，不显示分页按钮；加载期间仍可播放和收藏已有歌曲。
Esc/Home/F1 返回 Home 后继续播放并保留登录与队列，点击音乐卡片复用当前实例。应用目录的“关闭当前应用”才停止应用。
日推每次使用 `afresh=true` 请求新推荐，每轮追加最多 30 首，按歌曲 ID 去重；不足时最多请求 3 次补足。
若服务端没有返回新歌曲，会保留当前列表并提示刷新，停止自动请求。刷新推荐时也会过滤最近已见的歌曲。
刷新只得到少量新歌时，新歌置顶，再从原推荐补足最多 30 首；不会用 1–2 首新歌覆盖原来的整页。
界面区分“更新数量”和“列表总数”；触底追加始终只添加新歌，不使用旧歌补齐。
终端的 `NETEASE_RECOMMEND response/publish` 日志记录原始返回、有效歌曲、新增和保留数量，不输出认证凭证。
追加内容保留滚动位置；若当前播放队列来自该列表，新内容也会接到队列末尾。
系统主音量由 Host 管理，应用不实现全局音量控件。

点右上角“登录”也可打开微信二维码页。认证使用网易云官网的网页 `web2` 流程，
不使用 Android/iOS/TV 特殊客户端，不需要自建微信 AppID/AppSecret 或登录网关。
手机微信确认后，Guest 用网易云返回的 Cookie 查询账号，成功后搜索/播放请求携带同一会话。
微信账号需已关联网易云账号；若官网要求绑定手机号或额外验证，界面会提示在官网完成。

二维码支持过期、取消、刷新和返回；账户页显示昵称并提供退出登录。
微信状态查询允许等待 35 秒；控制台输出 `NETEASE_LOGIN poll response` 状态码与耗时，
408 表示等待扫码，404 表示已扫码，405 表示手机已确认。网络查询失败会在界面提示并重试。
“记住登录”默认关闭；开启后认证凭证保存在该 AppId 的本机 KV，PC SQLite **未加密**。
重启后重新验证账号，失效或退出时删除本机保存的凭证。登录不会绕过版权或会员限制。

`main.cpp` 是应用状态与事件循环，`music_view.hpp` 是列表/迷你播放器/播放页，`client.hpp` 是网易云请求与响应逻辑，`weapi.cpp` 是协议编码，
`json.hpp` 是有深度和节点上限的应用内 JSON 读取器。
`login.hpp` / `login_view.hpp` 管理微信授权与界面，`account.hpp` / `web.hpp` 管理会话和域名隔离。通用 HTTP/media 封装位于
[SDK](../../sdk/README.md)，Host 只负责网络传输及音频解码输出。

当前平台适配器为 PC。移到设备需实现 Network 1.2、Resource 1.1 图片解码、Audio 1.1 URL media 和 Input 1.2，
并提供共享中文系统字体；仅编译成 RV32 AOT 不会补齐设备端服务。
参考 API 项目的 MIT 版权见仓库 `THIRD_PARTY_NOTICES.md`。

使用与 Host 匹配的 WAMRC 2.4.3，可按标准 SDK 生成 release Bundle：

```sh
WASI_SDK_PATH=/opt/wasi-sdk WAMRC=/path/to/locked/wamrc \
  python3 tools/micropixel package guest/apps/netease-music \
  --profile release --aot-target riscv32-ilp32f --output-dir build/pc-netease-release
bash tools/pc.sh rv32-build
bash tools/pc.sh run --autostart build/pc-netease-release/netease-music.bundle.bin
```

这在 PC 的 QEMU RV32 运行程序中执行 AOT，网络和音频仍经 ABI 调用 PC Host。
可用以下入口验证 SDK wire 参数、加密与 JSON、真实音频输出和界面交互：

```sh
bash tools/tests/test_firmware_host.sh --pc --music
bash tools/tests/test_firmware_host.sh --pc --music --interactive
# 个人主页、歌单/推荐、滚动、播放、退出和失效
bash tools/tests/test_firmware_host.sh --pc --home --interactive
bash tools/tests/test_firmware_host.sh --pc --widgets
bash tools/tests/test_firmware_host.sh --pc --feed
# 接口返回 30 首但仅 2 首为新歌、刷新/切页、部分追加
bash tools/tests/test_firmware_host.sh --pc --feed --refresh
# 已生成上述 release Bundle 时，重复验证 RV32 版本
bash tools/tests/test_firmware_host.sh --pc --music --rv32
bash tools/tests/test_firmware_host.sh --pc --feed --rv32
```

微信登录专项回归使用本地模拟服务器，不需要真实账号：

```sh
bash tools/tests/test_firmware_host.sh --pc --login
bash tools/tests/test_firmware_host.sh --pc --login --interactive
# 覆盖超过旧 10 秒限制的延迟响应
bash tools/tests/test_firmware_host.sh --pc --login --long-poll
# 具备低端口绑定权限且能独占本机端口 80 时，覆盖默认 Host 与 RV32 长轮询
bash tools/tests/test_firmware_host.sh --pc --login --rv32 --long-poll --default-port
# release Bundle 已生成时，也可覆盖 RV32 SDK 调用
bash tools/tests/test_firmware_host.sh --pc --login --rv32
bash tools/tests/test_firmware_host.sh --pc --home --rv32
```

真实微信授权的最后一步需用户本人在手机上确认，自动测试只模拟该确认结果。
官网入口参考：[网易云网页微信登录](https://music.163.com/api/sns/authorize?snsType=10&clientType=web2&callbackType=Login&forcelogin=true)。

个人接口参考 api-enhanced 的 [用户歌单](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/user_playlist.js)、
[每日推荐](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/recommend_songs.js) 和
[歌单歌曲](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/playlist_track_all.js)。
这些调用在 Guest 中直接实现，运行时不启动参考项目的服务。歌单每批 20 个，歌曲详情每批 20 首，
日推每轮最多追加 30 首；歌单索引最多保留前 2000 首。HTTP 响应仍限制为 256 KiB，超出容量会提示读取失败。
头像仅接受网易云官方 HTTPS 地址，最多 64 KiB / 512×512，失败时使用默认图案。

音乐内容组织参考[网易云官方手机版](https://apps.apple.com/cn/app/id590338362)，视觉与布局见
[MixPad 4 面板设计](../../../docs/design/panel-ui.zh-CN.md)，通用控件与输入契约见
[Guest UI 控件](../../../docs/design/guest-ui-widgets.zh-CN.md)。喜欢状态最多同步 20000 个 ID，仍受 256 KiB 响应限制。
收藏相关参考：[喜欢/取消喜欢](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/like.js)、
[喜欢状态列表](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/likelist.js)。

单个滚动列表最多保留 600 项，达到容量时提示刷新重新开始；推荐历史最多保留本次会话最近 4096 个 ID。
这两个缓存及推荐列表在退出或切换账号时清除。


## Activity / PlaybackService（0.5.0）

页面资源由 MusicActivity 持有，账号、缓存和队列在 MusicModel 中保留；PlaybackService 负责解析、媒体和自动下一首。
隐藏页面不提交帧，销毁后再打开不会重登或重建播放队列。应用要求 Lifecycle 1.0 与 Network 1.3；
页面和 Service 各有独立 HTTP 通道，二维码长轮询不会阻塞下一首。
详见[D4 交付说明](../../../docs/development/app-manager-d4.zh-CN.md)。

`music_view.hpp` 使用公共 AppBar、SegmentedControl、VirtualList 和 ContentStateView。
列表在创建时固定分配 8 个 ListItem，以歌曲/歌单 ID 和数据代次校验动作；替换数据取消旧捕获，
追加保留滚动位置。页面继续负责分页、收藏确认、播放器和队列，公共组件只返回交互动作。
