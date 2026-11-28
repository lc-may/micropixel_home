# 蓝牙音箱

「蓝牙音箱」是 Orion 的独立 Guest。Host 接收和解码 A2DP SBC 并输出到板载扬声器。应用显示音频连接状态并通过 AVRCP 控制音源端的播放、暂停和切歌。音源支持播放位置和时长查询时，页面显示播放进度。

手机的媒体音量通过 AVRCP 调节蓝牙音频通道。Orion 的系统音量调节整机输出。

在「配置 → 经典蓝牙」选择接收模式并开启可发现，然后从 Android 或 Windows 选择 `MicroPixel Orion` 作为媒体音频输出。在音源端开始播放后打开「蓝牙音箱」，待遥控连接就绪，再测试应用的上一首、播放或暂停和下一首。返回桌面或打开其他应用时，接收音频和后台状态更新继续。

需要支持 Bluetooth Classic Guest ABI 1.2 的 Orion Host。启用 A2DP 的 Orion 出厂应用镜像已预装蓝牙音箱，应用也可以作为独立 Bundle 导入。

```sh
python3 tools/micropixel package guest/apps/bluetooth-speaker \
  --profile release --aot-target riscv32-ilp32d --output-dir build/orion-bluetooth-speaker/guest
python3 firmware/bouffalo/tools/orion_apps.py --port "$ORION_PORT" \
  import build/orion-bluetooth-speaker/guest/bluetooth-speaker.bundle.bin
```

在「全部应用 → 应用管理」确认安装。停止接收可从音源端断开音频，或在「配置 → 经典蓝牙」断开连接。
