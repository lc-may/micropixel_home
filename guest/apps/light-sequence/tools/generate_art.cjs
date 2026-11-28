// SPDX-License-Identifier: Apache-2.0
// Rasterize the supplied design's fixed-size artwork and typography. Requires
// Playwright with Chromium, Sharp and Noto Sans CJK SC on the development machine.
const fs = require("node:fs/promises");
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const { chromium } = require("playwright");
const sharp = require("sharp");
const { title } = require("../app.json");

const output = path.join(__dirname, "../assets");
const reference = path.join(__dirname, "../../LightSequence_480x480.html");
const palette = {
  Ink: "#f8f4ed", Sub: "#adaaa5", Quiet: "#92918f", Gold: "#f8cc90", Dark: "#2d2114",
  Power: "#e5e2de", Disabled: "#a5a4a2", RoomWarm: "#ffdbab", RoomCold: "#c7c7cc",
  RoomArrow: "#c1b8ae", RowWarm: "#ffdfa9", RowCold: "#c8c8cc", RowArrow: "#99999f",
  Offline: "#b2a49a", Tab: "#bab9b6", Selected: "#ffe0b5", Tag: "#e2d9cf",
  OrbOff: "#bbb8b2", Control: "#c3b8aa", Info: "#c7b49c", Check: "#2c2220",
};

const numbers = "0123456789";
const count = numbers + "/ ·已亮离线盏";
const places = "客厅主卧餐玄关";
const fixtures = "客厅主灯氛围灯带左右射主卧吸顶阅读壁床头餐桌吊餐边柜玄关鞋组";
const modes = "开关灯亮度可调冷暖光彩色 +";
const power = "全开关灯离线";
const tab = "单灯组白光彩";
const readout = numbers + " %K°";
const roles = [
  ["RoomTitle", 22, 690, 1.15, palette.Ink, places],
  ["Note", 12, 400, 1.4, palette.Sub, count + places + modes + " /区域控制灯组"],
  ["Count", 15, 700, 1.4, palette.Ink, numbers + " /"],
  ["CountTail", 13, 400, 1.4, palette.Sub, " 盏已亮"],
  ["Heading", 26, 720, 1.14, palette.Ink, fixtures + places],
  ["Status", 15, 680, 1.4, palette.Ink, "本区灯光组内"],
  ["Ledger", 16, 690, 1.4, palette.Ink, "组内灯具"],
  ["RowTitle", 15, 620, 1.2, palette.Ink, fixtures],
  ["RowNote", 11, 400, 1.2, palette.Sub, count + modes + "已关闭开启彩光% K"],
  ["OfflineNote", 11, 400, 1.2, "#e4bd9a", "离线 · 无法控制"],
  ["Tab", 15, 400, 1.4, palette.Tab, tab],
  ["TabSelected", 15, 660, 1.4, palette.Selected, tab],
  ["PreviewStatus", 12, 510, 1.4, "#c3b9ae", "连接恢复后可控制开灯上次设置光预览"],
  ["PreviewValue", 27, 735, 1.23, palette.Ink, numbers + ", K#ABCDEF%已开启"],
  ["PreviewOff", 22, 735, 1.23, palette.Ink, "设备离线灯光已关闭"],
  ["PreviewTag", 11, 400, 1.4, palette.Tag, "彩光白已开关离线"],
  ["ControlLabel", 14, 615, 1.4, palette.Ink, "亮度色温相饱和"],
  ["BrightnessRead", 14, 675, 1.4, "#fff0d9", readout],
  ["SettingRead", 13, 675, 1.4, "#fff0d9", readout],
  ["Preset", 13, 400, 1.4, "#e6e1db", "暖白自然冷"],
  ["PresetSelected", 13, 400, 1.4, "#ffe1b5", "暖白自然冷"],
  ["Kelvin", 10, 400, 1.4, "#c2b7a8", numbers + " K"],
  ["Capability", 13, 400, 1.4, palette.Sub, "此灯仅支持开关与亮度"],
  ["Toast", 13, 400, 1.4, "#fff0dc", numbers + " 已开关盏灯在线全部闭启·跳过离"],
];
const detailStart = roles.findIndex(role => role[0] === "PreviewStatus");
const powerStart = roles.length;
for (const [name, size] of [["RoomPower", 11], ["RowPower", 12], ["Power", 13]]) {
  for (const tone of ["Power", "Gold", "Dark", "Disabled"]) {
    roles.push([name + tone, size, 400, 1, palette[tone], power]);
  }
}
const textAtlas = role => role < detailStart ? 0 : role < powerStart ? 1 : 2;

const iconSpecs = [
  [["sofa", "bed", "dine", "door"], 25, ["RoomWarm", "RoomCold"]],
  [["arrow"], 17, ["RoomArrow"]],
  [["back"], 22, ["Ink"]],
  [["sun"], 17, ["Gold"]],
  [["sun", "layers"], 21, ["Gold"]],
  [["power", "offline"], 17, ["Power", "Gold", "Dark", "Disabled"]],
  [["power", "offline", "sun", "moon"], 15, ["Power", "Gold", "Dark", "Disabled"]],
  [["bulb", "layers", "sun", "color"], 17, ["Tab", "Selected"]],
  [["ceiling", "strip", "spot", "dine", "layers"], 19, ["RowWarm", "RowCold"]],
  [["arrow"], 15, ["RowArrow"]],
  [["offline"], 19, ["Offline"]],
  [["ceiling", "strip", "spot", "dine"], 24, ["Dark", "OrbOff"]],
  [["color", "sun", "moon", "offline"], 13, ["Tag"]],
  [["sun", "temp"], 16, ["Control"]],
  [["info"], 18, ["Info"]],
  [["info"], 16, ["Gold"]],
  [["check"], 15, ["Check"]],
];

const namedShapes = [
  ["RoomCold", 214, 140, '<article class="room"></article>'],
  ["RoomWarm", 214, 140, '<article class="room" data-warm="true"></article>'],
  ["Ribbon", 440, 70, '<section class="room-status" style="margin:0"></section>'],
  ["Empty", 440, 108, '<div class="empty">此区域尚未配置灯组</div>'],
];
const lightShapes = [
  ["TemperatureTrack", 412, 11, '<div style="border-radius:7px;background:linear-gradient(to right,#eaac67,#f5ecd9 48%,#a7c6f8)"></div>'],
  ["HueTrack", 198, 11, '<div style="border-radius:7px;background:linear-gradient(to right,#ff5252,#ffee62,#61e875,#5ce9f0,#666cfb,#e871f5,#ff5252)"></div>'],
  ["SaturationCover", 198, 11, '<div style="border-radius:7px;background:linear-gradient(to right,#f4efeb,transparent)"></div>'],
  ["OrbCover", 59, 59, '<span class="orb" style="--beam:transparent;box-shadow:none"></span>'],
];

const crops = [
  ["Brand", 20, 22, 280, 30],
  ["Demo", 385, 24, 75, 26],
  ["Intro", 21, 62, 118, 62],
  ["Footer", 20, 440, 440, 22],
];

function pack(items, gap = 1) {
  const width = 512;
  const rows = [];
  let height = 0;
  for (const item of [...items].sort((a, b) => b.height - a.height || b.width - a.width)) {
    let row = rows.find(row => row.x + item.width <= width && row.height >= item.height);
    if (!row) {
      row = {x:0, y:height ? height + gap : 0, height:item.height};
      rows.push(row);
      height = row.y + row.height;
    }
    item.x = row.x;
    item.y = row.y;
    row.x += item.width + 1;
  }
  if (height > 512) throw new Error("The atlas exceeds the Orion texture limit: " + height);
  return {width, height};
}

async function previewCover() {
  const width = 438, height = 84;
  const pixels = Buffer.alloc(width * height * 4);
  const unit = x => Math.min(1, Math.max(0,x));
  for (let y = 0; y < height; ++y) for (let x = 0; x < width; ++x) {
    const px = x + 0.5, py = y + 0.5;
    const radial = unit(1 - Math.hypot((px - 48.18) / 386, (py - 42) / 41.6));
    const shadow = unit((59 - Math.hypot(px - 47.5, py - 42)) / 57);
    const weight = (radial + shadow - 0.5 * radial * shadow) / 1.5;
    const coverage = unit(16.5 - Math.hypot(px - Math.min(width - 16, Math.max(16,px)),
                                           py - Math.min(height - 16, Math.max(16,py))));
    const i = (y * width + x) * 4;
    pixels[i] = 36;
    pixels[i+1] = 37;
    pixels[i+2] = 41;
    pixels[i+3] = Math.round(coverage * (1 - weight) * 255);
  }
  return {name:"PreviewCover", width, height,
    input:await sharp(pixels,{raw:{width,height,channels:4}}).png().toBuffer()};
}

function place(item, child) {
  const bounds = "position:absolute;left:" + item.x + "px;top:" + item.y + "px;width:" +
    item.width + "px;height:" + item.height + "px;";
  return '<div style="' + bounds + '">' + child + "</div>";
}

function styleText(role) {
  const [, size, weight, lineHeight, color] = role;
  return "font:" + weight + " " + size + "px/" + lineHeight + " var(--sans);color:" + color +
    ";white-space:pre;font-variant-numeric:tabular-nums";
}

function escaped(value) {
  return value.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

function rect(item) {
  return "{" + [item.x, item.y, item.width, item.height].join(", ") + "}";
}

async function savePng(name, buffer) {
  // Orion applies its 64 KiB decoder limit to PNGs loaded from a Bundle too.
  if (buffer.length > 64 * 1024) throw new Error(name + " exceeds the Orion PNG byte limit: " + buffer.length);
  await fs.writeFile(path.join(output, name), buffer);
}

async function run() {
  const executablePath = process.env.LIGHT_SEQUENCE_CHROMIUM || undefined;
  const browser = await chromium.launch({executablePath, args:["--no-sandbox"]});
  try {
    const page = await browser.newPage({viewport:{width:480,height:480}});
    await page.goto(pathToFileURL(reference).href);
    await page.locator(".brand").evaluate((brand, title) => {
      const name = [...brand.childNodes].find(node => node.nodeType === Node.TEXT_NODE && node.textContent.trim());
      name.textContent = title;
      brand.querySelector(".english").textContent = "WHOLE-HOME LIGHTING";
    }, title);
    await page.evaluate(() => document.fonts.ready);
    const artwork = [];
    for (const [name, x, y, width, height] of crops) {
      artwork.push({name, width, height, input:await page.screenshot({clip:{x, y, width, height}})});
    }
    const strokes = await page.evaluate(() => window.eval("strokes"));
    const icons = [];
    for (const [names, size, tones] of iconSpecs) {
      for (const icon of names) for (const tone of tones) {
        const svg = '<svg width="' + size + '" height="' + size + '" viewBox="0 0 24 24" fill="none" stroke="' +
          palette[tone] + '" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">' +
          strokes[icon] + '</svg>';
        icons.push({icon, tone, width:size, height:size, markup:svg});
      }
    }
    const roleChars = roles.map(role => [...new Set(role[5])].sort());
    const widths = await page.evaluate(rows => {
      const content = document.createElement("span");
      document.body.append(content);
      const output = rows.map(([style, chars]) => {
        content.style.cssText = "position:fixed;visibility:hidden;" + style;
        return chars.map(c => {
          content.textContent = c;
          return Math.round(content.getBoundingClientRect().width * 64);
        });
      });
      content.remove();
      return output;
    }, roles.map((role, i) => [styleText(role), roleChars[i]]));
    const glyphs = roles.flatMap((role, i) => roleChars[i].map((character, j) => {
      const advance = widths[i][j];
      const height = Math.ceil(role[1] * role[3]);
      return {character, advance, width:Math.ceil(advance / 64) + 2, height, atlas:textAtlas(i),
        markup:'<span style="position:absolute;left:1px;top:0;' + styleText(role) + '">' +
          escaped(character) + '</span>'};
    }));

    await page.setViewportSize({width:512,height:512});
    await page.addStyleTag({content: "html,body {background:transparent!important} .frame {display:none}"});
    await page.evaluate(() => {
      const art = document.createElement("div");
      art.id = "art";
      art.style.cssText = "position:fixed;inset:0";
      document.body.append(art);
    });

    const render = async (items, extent) => {
      await page.locator("#art").evaluate((el, html) => {el.innerHTML = html;},
        items.filter(i=>i.markup).map(i=>place(i,i.markup)).join(""));
      await page.evaluate(() => document.fonts.ready);
      return page.screenshot({clip:{x:0,y:0,...extent}, omitBackground:true});
    };
    await savePng("icons.png", await render(icons, pack(icons)));
    for (const [i, name] of ["text_navigation.png", "text_detail.png", "text_power.png"].entries()) {
      const items = glyphs.filter(glyph => glyph.atlas === i);
      await savePng(name, await render(items, pack(items)));
    }

    const lightArt = [await previewCover()];
    function addShapes(target, shapes) {
      for (const [name, width, height, html] of shapes) {
        target.push({name, width, height, markup:html.replace(/^(<\w+)([^>]*?)>/,
        (_match, tag, attrs) => {
          const size = "width:100%;height:100%;";
          return tag + (attrs.includes('style="') ? attrs.replace('style="','style="'+size) : attrs+' style="'+size+'"')+'>';
        })});
      }
    }
    addShapes(artwork, namedShapes);
    addShapes(lightArt, lightShapes);
    for (const [i, [items, name]] of [[artwork, "surfaces.png"], [lightArt, "lighting.png"]].entries()) {
      for (const item of items) item.atlas = i;
      const base = await render(items, pack(items, 0));
      const combined = await sharp(base).composite(items.filter(i=>i.input).map(i=>({input:i.input,left:i.x,top:i.y})))
        .png().toBuffer();
      await savePng(name, combined);
    }
    const surfaces = [...artwork, ...lightArt];

    const iconName = name => "k" + name[0].toUpperCase() + name.slice(1);
    const enumList = values => values.map(value => "k" + value).join(", ");
    let start = 0;
    const ranges = roleChars.map((chars, i) => {
      const range = "{" + start + ", " + chars.length + ", " + textAtlas(i) + "}";
      start += chars.length;
      return range;
    });
    const header = [
      "// SPDX-License-Identifier: Apache-2.0",
      "// Coordinate map for the app-local artwork atlases.",
      "#pragma once", "", "#include <string_view>", "", '#include "sdk/geometry.hpp"', "",
      "namespace light_sequence::art_data {",
      "enum class Surface { " + enumList(surfaces.map(i=>i.name)) + " };",
      "enum class Icon { " + [...new Set(icons.map(i=>i.icon))].map(iconName).join(", ") + " };",
      "enum class Tone { " + enumList(Object.keys(palette)) + " };",
      "enum class Text { " + enumList(roles.map(r=>r[0])) + " };", "",
      "struct SurfaceItem { micropixel::Rect rect; int atlas; };",
      "inline constexpr SurfaceItem kSurfaces[] = {",
      ...surfaces.map(item => "    {" + rect(item) + ", " + item.atlas + "},  // " + item.name), "};", "",
      "struct IconItem { Icon icon; Tone tone; micropixel::Rect rect; };",
      "inline constexpr IconItem kIcons[] = {",
      ...icons.map(item => "    {Icon::" + iconName(item.icon) + ", Tone::k" + item.tone + ", " + rect(item) + "},"), "};", "",
      "struct Glyph { std::string_view text; micropixel::Rect rect; int advance64; };",
      "inline constexpr Glyph kGlyphs[] = {",
      ...glyphs.map(item => "    {" + JSON.stringify(item.character) + ", " + rect(item) + ", " + item.advance + "},"), "};", "",
      "struct RoleRange { int start, length, atlas; };",
      "inline constexpr RoleRange kRoles[] = {",
      ...ranges.map((range, i) => "    " + range + ",  // " + roles[i][0]), "};",
      "}  // namespace light_sequence::art_data", "",
    ].join("\n");
    await fs.writeFile(path.join(output,"art_data.hpp"),header);
  } finally {
    await browser.close();
  }
}
run().catch(error => {console.error(error); process.exitCode = 1;});
