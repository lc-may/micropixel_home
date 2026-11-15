# Espressif 固件

Linux/WSL2 桌面目标见 [PC Host](pc/README.zh-CN.md)：运行原 Wasm 应用，使用 LVGL/SDL、
SDL 音频及 POSIX socket、HTTP/HTTPS、MQTT，无需 ESP-IDF。

使用 ESP-IDF 6.1 与固定版本的 MicroPixel WAMR fork。在仓库根目录执行命令，
先按[构建指南](../docs/development/flashing.zh-CN.md)配置工具链。

```sh
bash tools/p4.sh build-host
bash tools/s31.sh build-host
bash tools/s3.sh build-host <box3|szpi|cores3|watcher>
python3 tools/firmware.py <ksdiy-p4c5|ksdiy-p4c5-landscape> build
```

构建与烧录分开执行。例如 `bash tools/p4.sh flash-host <port>` 烧录已构建的 Host，保留 App Store。
连接前关闭其他串口工具。

- [构建、配置与烧录](../docs/development/flashing.zh-CN.md)
- [源码导航](espressif/main/README.zh-CN.md)
- [参与贡献](../CONTRIBUTING.zh-CN.md)
- [English](README.md)

网易云入口现在运行 [SDK Guest 应用](../guest/apps/netease-music/README.zh-CN.md)：
`bash tools/pc.sh run --music` 会增量构建并启动 Wasm。通用服务边界见
[网络与音频设计](../docs/design/pc-music.zh-CN.md)。
