# Material panel icons

These SVG sources are Google **Material Symbols Rounded** (48 px optical size),
from [google/material-design-icons](https://github.com/google/material-design-icons),
commit `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7`.
Upstream path: `symbols/web/<name>/materialsymbolsrounded/<name>_48px.svg`.
Copyright Google LLC, licensed under [Apache-2.0](../../../../LICENSE).

`tools/build_panel_icons.py` applies the panel palette and generates the
bounded atlas in `material_icons_data.hpp`. Home, weather, clock/calendar and news share it.
Sunny, partly cloudy, cloudy, fog, drizzle, rain, snow, thunderstorm and unavailable
states have different shapes. Mapping uses Open-Meteo's WMO codes; unknown values
show the unavailable icon. No network fetch is needed at runtime.

The 33 symbols also cover alarm, calendar, Notes, music, news, apps, navigation,
refresh, volume and playback/edit controls. Use `ui::MaterialAtlas` once per
Activity, then share it between `ui::MaterialIcon` and `ui::MaterialIconButton`.
The SVG geometry stays upstream; the generator applies white, amber, blue and
muted gray from the panel palette. The generator requires Pillow and CairoSVG.

```sh
python3 tools/build_panel_icons.py --source guest/sdk/ui/icons \
  --output guest/sdk/ui/material_icons_data.hpp
clang-format -i guest/sdk/ui/material_icons_data.hpp
```

The adjacent numeric atlas uses Noto Sans SemiBold (SIL OFL 1.1, see
[license](../NotoSans-OFL.txt)). Regenerate it with Pillow and a matching source
font; the header records the source SHA-256:

```sh
python3 tools/fonts/build_panel_numerals.py --font /path/to/NotoSans-SemiBold.ttf \
  --output guest/sdk/ui/numerals_data.hpp
clang-format -i guest/sdk/ui/numerals_data.hpp
```
