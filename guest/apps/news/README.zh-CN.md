# 新闻热榜

按照 [720×720 设计稿](../../../docs/ui_design/index.html)的 04 新闻和 15 加载态实现的 Guest Activity。
使用标准 MicroPixel Scene、ScrollView、Network、Storage 和 Lifecycle SDK；不需要 API 网关或登录。

运行 `bash tools/pc.sh run --home`，在“应用”中打开“新闻热榜”。`--home --rv32` 使用相同界面的 checked RV32 AOT。
也可单独构建与运行：

```sh
bash tools/pc.sh guest guest/apps/news
bash tools/pc.sh run --autostart build/pc-apps/news/news.pc.json
```

## 内容与界面

直连[今日头条热榜接口](https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc)，显示 `data[].Title` 和
`HotValue`，保留接口顺序，最多 50 条。顶部显示北京时间的本次更新时间，右上角刷新。
三种本地视图为热榜、未读、已读；点击卡片查看完整标题、排名和热度，并标记为已读。
接口没有正文和可靠的新闻分类，本版提供标题速览。卡片右侧使用实际排名徽章。

卡片为 672×161、圆角 24、间距 15，标题采用 26 px、最多两行和省略号，右侧徽章 96×96、圆角 18。
滚动复用 4 个卡片，拖动松手不会打开新闻；首次加载显示骨架，失败显示缓存或重试提示。
缓存前 12 条热榜和最多 50 个已读标识，分别使用一个不超过 4 KiB 的 KV 值；重新打开应用后保留。
成功的空列表清除旧列表，HTTP/JSON 错误保留上次成功内容。

新闻没有后台 Service；隐藏时取消未完成请求并停止轮询，返回时按需重试。
天气、时钟、音乐和新闻可同时占用四个 Session，浏览新闻或关闭新闻都不停止后台音乐。
Job 1.0 仅用于读取 Host 的墙上时钟，不注册后台任务。

## 边界与验证

网络请求上限 256 KiB，分块读取；JSON 最多 16384 个 token；单条标题最多 255 个 UTF-8 字节。
无效记录被跳过，重复标题去重，全无有效记录的非空响应视为失败。
测试参数 `--news-origin` 可指定完整 fixture URL；默认始终使用上面的公开 HTTPS 接口。

```sh
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --news
bash tools/tests/test_firmware_host.sh --pc --news --rv32
bash tools/tests/test_firmware_host.sh --pc --news --live
bash tools/tests/test_firmware_host.sh --pc --news --rv32 --live
```

自动测试覆盖 130 KiB 响应、骨架、标题折行、省略号、滚动、速览、已读过滤、空列表、503 缓存、
请求取消、关闭后恢复缓存，以及四应用共存时的非静音 PCM。截图位于忽略的 `build/news-tests/` 和
`build/news-rv32-tests/`。`--live` 使用 Guest 的真实网络适配器访问今日头条，不使用 fixture。
