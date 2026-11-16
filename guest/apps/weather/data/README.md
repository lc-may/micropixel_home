# Offline city locations

`china_cities.json` is the location data from
[MCPKerykeion](https://github.com/wuunicorn/MCPKerykeion/blob/60cfefad90031d5f2e629031de90eef662d1409b/china_cities.json),
commit `60cfefad90031d5f2e629031de90eef662d1409b`, under the retained [MIT license](cities-MIT.txt).
It contains 394 cities/locations grouped into 34 provinces/regions. It is a
weather location catalog, rather than an authoritative administrative register.
Trailing whitespace is normalized. Only names and coordinates are used; no upstream application code is included.

The generator places ten common cities first: Shanghai, Beijing, Guangzhou,
Shenzhen, Hangzhou, Chengdu, Wuhan, Nanjing, Chongqing and Xi'an. This is a curated
shortcut order, rather than a usage ranking. Remaining locations keep source order.
Runtime weather requests use Open-Meteo with these coordinates; choosing a city
does not require a geocoding request, credentials or a gateway.

```sh
python3 tools/generate_weather_cities.py --source guest/apps/weather/data/china_cities.json \
  --output guest/apps/weather/cities.hpp
clang-format -i guest/apps/weather/cities.hpp
```

Changing the catalog order requires a cache version change. Version 4 intentionally
resets the previous three-city selection and cached weather.
