# Home and default app screenshots

These 720×720 PNGs are unmodified captures of the MicroPixel PC Host. Both root READMEs share these files.
Music, weather and reminder screens use local sample data. News screens show public CCTV content captured on 2026-11-17.

| File | Screen |
|---|---|
| `home-panel.png` | Home dashboard with weather, reminders, background music controls and four configured apps |
| `music-list.png` | Daily recommendations, favorites and mini player |
| `music-player.png` | Dedicated player with active playback and seeking |
| `weather-forecast.png` | Current, hourly and three-day forecasts |
| `weather-cities.png` | Region-based city selection |
| `clock-alarm.png` | Alarms and reminder summary |
| `clock-calendar.png` | Month calendar and date selection |
| `news-list.png` | CCTV headlines and read filters |
| `news-article.png` | Downloaded article and scrollable reading area |

The Home dashboard is captured by `bash tools/tests/test_firmware_host.sh --pc --news` with four configured apps.
Other panel screens are captured by `bash tools/tests/test_firmware_host.sh --pc --panel`;
the active music player by `--pc --music --interactive`; live article reading by `--pc --news --content --live`.
For a news list capture, run the news app with `--headless --seconds 14 --screenshot <output.png>` through
`bash tools/tests/test_firmware_host.sh --pc --run` after building the Guest.
Choose representative captures from the ignored `build/` output and copy the selected PNGs here.

News text originates from [CCTV News](https://news.cctv.com/) and remains the property of its respective rights holders.
The project's Apache-2.0 license does not relicense the third-party text visible within screenshots.
The screenshots include no audio files, account credentials or downloaded album artwork.
