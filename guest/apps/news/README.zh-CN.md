# 新闻与正文阅读

按照 [720×720 设计稿](../../../docs/ui_design/index.html)实现的 Guest Activity。
默认进入央视新闻频道，点击新闻卡片下载并阅读正文；点击左上“新闻 · 央视”可切换到保留的头条热榜。
使用标准 MicroPixel Scene、ScrollView、Network、Storage 和 Lifecycle SDK，不需要 API 网关、浏览器或登录。

在仓库根目录运行：

```sh
bash tools/pc.sh run --home
# 相同界面的 checked RV32 AOT
bash tools/pc.sh run --home --rv32
# 单独构建、运行新闻
bash tools/pc.sh guest guest/apps/news
bash tools/pc.sh run --autostart build/pc-apps/news/news.pc.json
```

## 数据来源

| 用途 | 公开地址与使用字段 |
|---|---|
| 央视新闻列表 | [央视新闻首页使用的 JSONP 列表](https://news.cctv.com/2019/07/gaiban/cmsdatainterface/page/news_1.jsonp)，`data.list[].title / url` |
| 央视新闻正文 | 列表中 `https://news.cctv.com/YYYY/MM/DD/ARTI….shtml` 的正文页面，提取 `content_area` 或页面内 `contentdate` 的文章数据 |
| 今日头条热榜 | [热榜接口](https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc)，`data[].Title / HotValue / ClusterIdStr` |

列表保留来源顺序，每个频道最多 50 条。正文通过 HTTPS 按需下载，提取真实文字，保留段落、中文和标点，
去除脚本、样式及视频代码；解析页面中已发布的字符串数据，执行过程中不运行 JavaScript。
正文页显示来源，支持上下滚动和右上角重试。纯视频条目明确提示没有文字正文。
头条热榜提供话题标题与热度，保持为话题速览；该页提供“阅读新闻正文”入口切换到央视频道。

## 界面与生命周期

列表使用 672×161、圆角 24、间距 15 的卡片，标题为 26 px、最多两行，提供未读/已读过滤。
复用 4 张卡片；正文复用 16 个文字节点，按每次最多 6 行分段排版，滚动时不创建新的图形节点。
详情卡片随内容增长，避免只有标题时显示固定的大块空白。

新闻没有后台 Service。隐藏或返回列表时取消未完成的正文请求，重新进入原文章可重试；
天气、时钟、音乐和新闻共用四个独立 Session，浏览、隐藏或关闭新闻都不停止后台音乐。
Job 1.0 仅用于读取 Host 时钟，不注册后台任务。

## 缓存与边界

两个频道各缓存前 12 条列表，保留最多 50 个已读标识与来源选择；旧版头条缓存和已读记录可以继续使用。
另外保存最近一篇文章的正文缓存，最多 3072 UTF-8 字节；超过该长度的离线缓存标为“节选”。
在线正文最多 32 KiB、768 行，达到上限会明确提示。每个缓存值均小于 4 KiB，更新失败保留上次内容。

列表和正文单次响应上限均为 256 KiB，分块读取；JSON 最多 16384 个 token，单条标题最多 255 字节。
正文只接受固定央视域名和经过校验的文章路径，不跟随页面中的任意外链；HTML、JSONP 均按数据解析。
单 App 同时只有一个请求，打开正文会取消正在刷新列表的请求。

联调参数通过 `--guest-arg` 传入：

- `--news-source cctv|toutiao`：指定本次启动的初始来源。
- `--news-origin URL`：头条列表 fixture 完整 URL。
- `--news-content-origin URL`：央视列表 fixture 完整 URL。
- `--news-article-origin URL`：正文 fixture 的基础地址，后接校验后的文章路径。

## 自动验证

```sh
bash tools/tests/test_firmware_host.sh --app-framework
bash tools/tests/test_firmware_host.sh --pc --news --content
bash tools/tests/test_firmware_host.sh --pc --news --content --rv32
bash tools/tests/test_firmware_host.sh --pc --news --content --live
bash tools/tests/test_firmware_host.sh --pc --news --content --rv32 --live
# 保留的头条热榜回归
bash tools/tests/test_firmware_host.sh --pc --news
bash tools/tests/test_firmware_host.sh --pc --news --rv32
```

测试覆盖列表/正文真实 HTTPS、HTML 与引用字符串解析、段落和实体、长文滚动、来源切换、
视频条目、失败/重试、离线节选、取消/恢复、关闭后恢复缓存，以及四应用共存时的非静音 PCM。
正文截图和日志位于忽略的 `build/news-content-tests/` 与 `build/news-content-rv32-tests/`。
`--live` 通过真实 Guest 网络 SDK 下载列表和第一篇文章，验证正文已在界面中显示。
