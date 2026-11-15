# 网易云 Guest 与 PC 服务

应用位于 `guest/apps/netease-music`，是普通的单线程 C++23 Wasm Guest。
搜索、分页、歌曲详情、weapi AES/RSA 编码、JSON 解析、权限提示和界面都在 Guest 中。
它只使用 MicroPixel SDK，不包含 LVGL、socket、mbedTLS、SDL 或系统调用。

个人主页同样在 Guest 中：验证登录后保留账号 ID、昵称、签名和头像 URL，用当前 Cookie/CSRF
访问用户歌单与每日推荐。歌单索引与歌曲详情分两步加载，详情每批 20 首，保留最多 2000 个 ID；
歌单每次读取 20 个、歌单歌曲详情每次 20 首，日推每轮最多追加 30 首，使用 8 个可复用列表项显示滚动内容。
触底后追加数据并保留偏移，不显示分页按钮。单个列表最多保留 600 项。JSON 默认 4096 token，
个人内容允许最多 32768 token，仍受 256 KiB 响应和 24 层深度限制。
播放队列与当前浏览列表分离。退出、切换账号和 301 失效会取消个人请求并清除列表、头像与队列。
头像使用不带 Cookie 的独立 Exchange，限制官方 HTTPS 地址和 64 KiB，再交给通用图片解码服务。

列表、迷你播放器和独立播放页复用 [Guest UI 控件](guest-ui-widgets.zh-CN.md)。
播放进度松手提交一次 Seek；歌曲红星通过 like 接口确认后更新，失败回读 liked IDs。
收藏写请求在页面导航时保留，读取任务排队等待；账号退出取消任务并清除所有个人状态。

推荐每次请求使用 `afresh=true`，按 ID 排除近期已见内容，并在有新内容但不足 30 首时最多请求 3 次补足。
历史缓存最多 4096 个 ID，最近一次推荐列表可在无新结果时保留；服务端只返回重复结果时停止自动加载。
刷新只获得部分新歌时，先放新歌，再从原推荐补齐最多 30 首；`added_items` 仅计新歌，保留数量单独记录。
追加不补旧歌；首次接口本身不足 30 首时按实际数量展示。每次响应与最终发布记录数量诊断，不记录会话或原始响应。
推荐、搜索、歌单与喜欢列表都在滚动手势结束且触底时加载，每个手势最多触发一次，加载中不重复请求。
追加失败保留已有内容和游标，下次上滑重试；同一数据集的播放队列随追加扩展，切换页面不替换队列。

Host 提供通用能力，不了解网易云协议：

| SDK | Service | PC 实现 |
| --- | --- | --- |
| `app.network().Start()/Exchange()` | Network 1.0/1.1/1.2，保留的 ID 19 | coreHTTP + socket/mbedTLS |
| `app.audio().OpenUrl()` | Audio 1.1，方法 15–20 | MediaPlayer + mpg123/Opus/WAV + SDL |
| `app.input().EnableText()/ReadText()` | Input 1.2，方法 2–3 | SDL UTF-8 输入，256 字节有界队列 |
| Scene / TextButton / Timer | 原有服务 | 共享 Guest 渲染器、系统中文字体 |

HTTP 每个 AppSession 只持有一个请求。START 复制 URL、header 行和 body 后立即返回 handle；
Guest 在事件循环中查询状态并按绝对 offset 读取。请求总长最多 8 KiB，URL 最多 2048 字节，
headers 最多 4096 字节/8 项，响应最多 256 KiB，每次读最多 4096 字节。
GET 暂不接受自定义 headers；POST 不跟随重定向。HTTPS 验证证书与主机名。

音频 URL handle 与 clip/PCM handle 独立。暂停、恢复、跳转和状态查询通过服务调用完成；
音量归 Host 控制。PC 先完整下载并解码，最多 32 MiB 压缩文件、128 MiB PCM，输出为
48 kHz、S16 立体声。SDL 回调不下载、解码或分配内存。

HTTP 与媒体各有一个有界待处理槽和一个可回收工作线程。CLOSE 取消任务，旧 generation 的结果
不会覆盖新任务。Session 销毁关闭持有的 handle；平台对象销毁取消任务并 join。
异步任务只持有 Host 副本，不借用 Guest 线性内存。这些服务沿用原有七个 Core imports，
RV32 桥可透传相同 wire 请求。

DeviceServices 的 HTTP/media 适配器可选；未实现的平台返回 `UNSUPPORTED`，不虚报播放能力。
当前只有 PC 实现这些平台适配器。应用 manifest 要求 Network 1.2 / Resource 1.1 / Audio 1.1 / Input 1.2，
不能直接声称支持原有 MCU 固件。设备端需要补齐相同服务后才能安装使用。

应用默认匿名直连 `music.163.com` 和网易云官方 CDN，不启动网关、不调用第三方音源、不绕过权限。
是否完整播放、试听或拒绝，以播放接口返回为准。`--test-origin` 只接受带端口的 loopback HTTP
地址，供自动化 fixture 使用；正常启动不传此参数。

## 微信网页登录

Guest 从网易云官网的 `/api/sns/authorize?snsType=10&clientType=web2&callbackType=Login&forcelogin=true`
发起微信授权，读取官方微信二维码图片和等待状态；手机确认后访问本次授权返回的网易云回调，
以 `/weapi/w/nuser/account/get` 的账号结果确认成功。`/login/qr/*` 属于网易云 App 扫码，不用于微信。
不收集微信密码，不使用特殊客户端版本、第三方登录网关或自建微信 AppSecret。

Network 1.1 追加显式 HTTP exchange、响应状态与响应头读取。原有 GET/POST 语义保持不变；
exchange 不自动跟随跳转，Guest 独立限制跳转次数及网易云/微信官方 HTTPS 域名。
Cookie 在 Guest 中按域、路径和 Secure 属性保存，不随跨域跳转转发；响应头最多 8 KiB。
所有账号凭证、OAuth code/state、Cookie 和二维码 nonce 均不写入日志。

Resource 1.1 追加 `Resources::DecodeTexture()`，同步读取最多 64 KiB PNG/JPEG，
在分配像素前将输入尺寸限制为 512×512，输出由 Host BitmapStore 持有并按普通 Texture 回收。
异步 HTTP 不保留 Guest 内存，解码调用返回前完成读取。

登录界面提供等待扫码、已扫码待确认、成功、拒绝、过期、刷新、返回和退出登录。
默认只在当前进程保存会话；选中“记住登录”后，认证 Cookie 保存到该 AppId 的私有 KV。
PC 的 SQLite KV 未加密。恢复的会话必须重新查询账号；失效或退出时清除本机保存的凭证。
登录成功不改变网易云的版权、会员或地域限制。

Network 1.2 追加带接收空闲时限的 HTTP exchange（方法 8，24 字节固定头），
`HttpOptions::receive_timeout_ms` 范围为 100–60000，默认仍为 10000。
PC 仅在该请求的接收工作线程等待数据，取消检查间隔不超过一次 socket poll；发送、连接、
TLS 和其他请求的时限保持原值。微信状态查询使用 35000 ms，覆盖官网长轮询等待。
HTTP Host 省略默认 80/443 端口，保留非默认端口与 IPv6 方括号。微信二维码服务会将 Host
带入二维码的确认地址，默认端口不能额外写入该地址。
轮询域名依官网 `usenewdomain` 标记选择已允许的 `lp.open.weixin.qq.com` 或旧 `long` 域名，
只记录状态码、耗时和脱敏错误，不记录二维码 nonce 或授权 code。
