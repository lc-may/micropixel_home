// SPDX-License-Identifier: Apache-2.0
#include "orion_home.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "bflb_common.h"
#include "bflb_mtimer.h"
#include "bflb_uart.h"
#include "display/orion_font.hpp"
#include "freertos/task.h"
#include "host/controller/media_widgets.hpp"
#include "host/ui/lvgl/home_layout.hpp"
#include "lv_port_disp.h"
#include "mm.h"
#include "runtime/abi/weather_endpoint.hpp"
#include "runtime/graphics/guest_panel.hpp"
#include "sdk/calendar.hpp"
#include "sdk/ui/material_icons_data.hpp"

extern "C" {
#include "lcd.h"
}

static_assert(LCD_INTERFACE_TYPE == LCD_INTERFACE_DSI, "An Orion LCD config must select a MIPI DSI panel");
static_assert(LCD_W == 480 && LCD_H == 480, "An Orion LCD config must select a 480 x 480 panel");
static_assert(LCD_COLOR_DEPTH == LV_COLOR_DEPTH, "The panel framebuffer format must match LVGL");

LV_FONT_DECLARE(font_orion_clock)
LV_FONT_DECLARE(font_orion_temperature)
LV_FONT_DECLARE(font_orion_home)
LV_FONT_DECLARE(font_orion_home_value)

namespace micropixel::bouffalo {
namespace art = ui::material_art;

namespace {

constexpr uint32_t kSymbolCount = art::Width * art::Height / (art::Cell * art::Cell);
constexpr uint32_t kWeatherSymbols = unsigned(art::Symbol::Unavailable) + 1;
constexpr unsigned kSmallIcon = 22, kWeatherIcon = 36, kControlIcon = 18;

template <unsigned Size>
struct IconTile final {
    std::array<uint8_t, Size * Size * 4> pixels;
    lv_image_dsc_t image;

    void Load(std::span<const uint8_t> original, unsigned symbol) {
        const unsigned top = symbol / art::Columns * art::Cell;
        const unsigned left = symbol % art::Columns * art::Cell;
        for (unsigned y = 0; y < Size; ++y) {
            const unsigned source_y = top + (2 * y + 1) * art::Cell / (2 * Size);
            for (unsigned x = 0; x < Size; ++x) {
                const unsigned source_x = left + (2 * x + 1) * art::Cell / (2 * Size);
                std::memcpy(pixels.data() + (y * Size + x) * 4,
                            original.data() + (source_y * art::Width + source_x) * 4, 4);
            }
        }
        image = {.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                            .cf = LV_COLOR_FORMAT_ARGB8888,
                            .w = Size,
                            .h = Size,
                            .stride = Size * 4},
                 .data_size = pixels.size(),
                 .data = pixels.data()};
    }
};

}  // namespace

struct OrionHome::IconStorage final {
    std::array<IconTile<kSmallIcon>, kSymbolCount> small;
    std::array<IconTile<kWeatherIcon>, kWeatherSymbols> weather;
    IconTile<kControlIcon> play, pause, next;
};

namespace {

namespace layout = host::home_layout;
std::atomic<uint32_t> scanout_frames;
void FrameReceived() { scanout_frames.fetch_add(1U, std::memory_order_relaxed); }

constexpr int Scale(int value) { return value * LCD_W / layout::kExtent; }
constexpr int DesignPixels(int value) { return value * layout::kExtent / LCD_W; }

void Panel(lv_obj_t* object, layout::Bounds bounds, uint32_t color, int radius = 20) {
    lv_obj_set_pos(object, Scale(bounds.x), Scale(bounds.y));
    lv_obj_set_size(object, Scale(bounds.width), Scale(bounds.height));
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, Scale(radius), 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_flag(object, LV_OBJ_FLAG_SCROLLABLE, false);
}

lv_obj_t* Label(lv_obj_t* parent, int x, int y, int width, const char* text, uint32_t color = layout::kText,
                const lv_font_t* font = nullptr) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, Scale(x), Scale(y));
    lv_obj_set_width(label, Scale(width));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font ? font : &font_orion_home, 0);
    return label;
}

lv_obj_t* Card(lv_obj_t* parent, layout::Bounds bounds, const char* title) {
    auto* card = lv_button_create(parent);
    Panel(card, bounds, layout::kSurface, 27);
    if (*title) Label(card, 24, 21, 279, title, layout::kMuted);
    return card;
}

lv_obj_t* Label(lv_obj_t* parent, layout::Bounds bounds, const char* text, uint32_t color,
                const lv_font_t* font = nullptr) {
    return Label(parent, bounds.x, bounds.y, bounds.width, text, color, font);
}

void TimeLabel(lv_obj_t* label, uint64_t utc, int32_t offset) {
    const auto minutes = utc ? calendar::LocalSeconds(utc, offset) / 60 % 1440 : 0;
    if (utc) {
        lv_label_set_text_fmt(label, "%02u:%02u", static_cast<unsigned>(minutes / 60),
                              static_cast<unsigned>(minutes % 60));
    } else {
        lv_label_set_text_static(label, "--:--");
    }
}

art::Symbol RoleIcon(HomeAppRole role) {
    switch (role) {
        case HomeAppRole::kWeather:
            return art::Symbol::PartlyCloudy;
        case HomeAppRole::kMusic:
            return art::Symbol::Music;
        case HomeAppRole::kClock:
            return art::Symbol::Calendar;
        case HomeAppRole::kNews:
            return art::Symbol::News;
        default:
            return art::Symbol::Apps;
    }
}

}  // namespace

bool OrionHome::InitializeIcons() {
    auto* storage = static_cast<IconStorage*>(kmalloc(sizeof(IconStorage), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (!storage) return false;
    constexpr size_t bytes = art::Width * art::Height * 4;
    auto* original = static_cast<uint8_t*>(kmalloc(bytes, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (!original || !art::Expand({original, bytes})) {
        kfree(original);
        kfree(storage);
        return false;
    }
    icons_ = std::construct_at(storage);
    const std::span<const uint8_t> atlas(original, bytes);
    for (unsigned i = 0; i < icons_->small.size(); ++i) icons_->small[i].Load(atlas, i);
    for (unsigned i = 0; i < icons_->weather.size(); ++i) icons_->weather[i].Load(atlas, i);
    icons_->play.Load(atlas, unsigned(art::Symbol::Play));
    icons_->pause.Load(atlas, unsigned(art::Symbol::Pause));
    icons_->next.Load(atlas, unsigned(art::Symbol::Next));
    kfree(original);
    return true;
}

lv_obj_t* OrionHome::Icon(lv_obj_t* parent, int x, int y, int size, unsigned index) {
    auto* image = lv_image_create(parent);
    const lv_image_dsc_t* source = &icons_->small[index].image;
    if (Scale(size) == kWeatherIcon)
        source = &icons_->weather[index].image;
    else if (Scale(size) == kControlIcon) {
        if (index == unsigned(art::Symbol::Play)) source = &icons_->play.image;
        if (index == unsigned(art::Symbol::Pause)) source = &icons_->pause.image;
        if (index == unsigned(art::Symbol::Next)) source = &icons_->next.image;
    }
    lv_image_set_src(image, source);
    lv_obj_set_pos(image, Scale(x), Scale(y));
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}

bool OrionHome::Initialize(Callbacks callbacks) {
    callbacks_ = callbacks;
    lv_init();
    lv_tick_set_cb([] { return static_cast<uint32_t>(bflb_mtimer_get_time_ms()); });
    display_ = lv_port_disp_init();
    if (!display_) return false;
    lcd_frame_callback_register(FRAME_INT_TYPE_CYCLE, FrameReceived);
    const uint64_t deadline = bflb_mtimer_get_time_ms() + 250U;
    while (!Frames() && bflb_mtimer_get_time_ms() < deadline) vTaskDelay(pdMS_TO_TICKS(5));
    if (!Frames()) {
        std::puts("Orion display scanout did not start");
        return false;
    }

    if (touch_.Initialize(LCD_W, LCD_H)) {
        auto* pointer = lv_indev_create();
        lv_indev_set_display(pointer, display_);
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(pointer, this);
        lv_indev_set_read_cb(pointer, ReadTouch);
    } else {
        std::puts("Orion touch initialization failed");
    }

    if (!InitializeIcons()) return false;
    home_ = lv_display_get_screen_active(display_);
    guest_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(guest_);
    lv_obj_set_flag(guest_, LV_OBJ_FLAG_SCROLLABLE, false);
    lv_obj_set_style_bg_color(guest_, lv_color_hex(layout::kBg), 0);
    lv_obj_set_style_bg_opa(guest_, LV_OPA_COVER, 0);
    auto* back = lv_label_create(guest_);
    Panel(back,
          {0, DesignPixels(GuestPanel::kContentHeight), layout::kExtent, DesignPixels(GuestPanel::kNavigationHeight)},
          layout::kSurface, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(back, lv_color_hex(layout::kRaised), LV_STATE_PRESSED);
    const auto* font = GuestPanel::Font(MICROPIXEL_SYSTEM_FONT_SMALL);
    lv_obj_set_style_pad_top(back, (GuestPanel::kNavigationHeight - font->line_height) / 2, 0);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(back, lv_color_hex(layout::kAmber), 0);
    lv_obj_set_style_text_font(back, font, 0);
    lv_label_set_text_static(back, LV_SYMBOL_LEFT "  返回上一页");
    lv_obj_add_event_cb(back, Back, LV_EVENT_CLICKED, this);

    Panel(home_, {0, 0, layout::kExtent, layout::kExtent}, layout::kBg, 0);
    lv_obj_set_style_text_font(home_, &font_orion_home, 0);

    Panel(lv_obj_create(home_), {24, 27, 9, 9}, layout::kAmber, 9);
    Label(home_, 48, 21, 380, "家中 · MicroPixel", layout::kMuted);
    auto* hero = lv_obj_create(home_);
    Panel(hero, layout::kHero, layout::kHeroStart, 30);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(layout::kHeroEnd), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);

    clock_card_ = Card(home_, layout::kClock, "");
    lv_obj_set_style_bg_opa(clock_card_, LV_OPA_TRANSP, 0);
    time_ = Label(home_, 54, 122, 430, "--:--", layout::kText, &font_orion_clock);
    date_ = Label(home_, layout::kClockDate, "请同步时间", layout::kMuted);

    weather_card_ = Card(home_, layout::kWeather, "");
    lv_obj_set_style_bg_opa(weather_card_, LV_OPA_TRANSP, 0);
    weather_city_ = Label(home_, layout::kWeatherCity, "天气", layout::kMuted);
    weather_value_ = Label(home_, 504, 163, 180, "--°", layout::kText, &font_orion_temperature);
    weather_condition_ = Label(home_, layout::kWeatherCondition, "等待天气更新", layout::kMuted);
    weather_icon_ = Icon(home_, layout::kWeatherIcon.x, layout::kWeatherIcon.y, layout::kWeatherIcon.width,
                         unsigned(art::Symbol::Unavailable));
    weather_button_ = Card(home_, layout::kRefresh, "");
    lv_obj_set_style_bg_opa(weather_button_, LV_OPA_TRANSP, 0);
    Icon(weather_button_, 25, 20, 34, unsigned(art::Symbol::Refresh));

    const auto& icon = layout::kCardIcon;
    alarm_card_ = Card(home_, layout::kAlarm, "闹钟");
    Icon(alarm_card_, icon.x, icon.y, icon.width, unsigned(art::Symbol::Alarm));
    alarm_ = Label(alarm_card_, 24, 69, 279, "--:--", layout::kText, &lv_font_montserrat_32);
    alarm_status_ = Label(alarm_card_, 24, 137, 279, "尚未设置", layout::kMuted);
    agenda_card_ = Card(home_, layout::kAgenda, "日历与事项");
    Icon(agenda_card_, icon.x, icon.y, icon.width, unsigned(art::Symbol::Calendar));
    agenda_date_ = Label(agenda_card_, 24, 76, 279, "今天", layout::kText, &font_orion_home_value);
    Label(agenda_card_, 24, 137, 279, "暂无待提醒事项", layout::kMuted);

    music_card_ = Card(home_, layout::kMusic, "");
    music_font_ = lv_font_montserrat_18;
    music_font_.fallback = &OrionFontFallback();
    Label(music_card_, 24, 17, 279, "音乐", layout::kMuted);
    music_title_ = Label(music_card_, 24, 51, 279, "网易云音乐");
    lv_obj_set_height(music_title_, Scale(36));
    music_status_ = Label(music_card_, 24, 127, 106, "选择音乐", layout::kMuted);
    auto* transport = lv_obj_create(music_card_);
    Panel(transport, layout::kTransport, layout::kRaised, 20);
    lv_obj_remove_flag(transport, LV_OBJ_FLAG_CLICKABLE);
    music_play_ = Card(music_card_, {135, 84, 84, 84}, "");
    music_next_ = Card(music_card_, {219, 84, 84, 84}, "");
    for (auto* button : {music_play_, music_next_}) {
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    music_play_background_ = lv_obj_create(music_play_);
    Panel(music_play_background_, {10, 18, 64, 48}, 0x78695c, 16);
    lv_obj_remove_flag(music_play_background_, LV_OBJ_FLAG_CLICKABLE);
    music_play_icon_ = Icon(music_play_, 28, 28, 28, unsigned(art::Symbol::Play));
    lv_obj_set_style_image_recolor(music_play_icon_, lv_color_hex(0x1a1305), 0);
    lv_obj_set_style_image_recolor_opa(music_play_icon_, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(music_play_icon_, LV_OPA_40, 0);
    music_next_icon_ = Icon(music_next_, 28, 28, 28, unsigned(art::Symbol::Next));
    lv_obj_set_style_image_opa(music_next_icon_, LV_OPA_40, 0);
    lv_obj_add_event_cb(music_play_, TogglePlayback, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_add_event_cb(music_next_, NextTrack, LV_EVENT_SHORT_CLICKED, this);
    auto* apps_card = Card(home_, layout::kApps, "应用");
    Icon(apps_card, icon.x, icon.y, icon.width, unsigned(art::Symbol::Apps));
    count_ = Label(apps_card, 24, 68, 279, "0", layout::kText, &lv_font_montserrat_32);
    Label(apps_card, 24, 137, 279, "全部应用与系统音量", layout::kMuted);
    lv_obj_add_event_cb(apps_card, ToggleCatalog, LV_EVENT_SHORT_CLICKED, this);
    Panel(lv_obj_create(home_), layout::kPageIndicator, layout::kAmber, 5);
    auto* catalog_page = lv_obj_create(home_);
    Panel(catalog_page, layout::kCatalogIndicator, 0x3a4248, 4);
    lv_obj_set_ext_click_area(catalog_page, Scale(23));
    lv_obj_add_event_cb(catalog_page, ToggleCatalog, LV_EVENT_SHORT_CLICKED, this);
    status_ = Label(home_, 24, 670, 672, "", layout::kMuted);
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);

    catalog_overlay_ = lv_obj_create(home_);
    Panel(catalog_overlay_, {0, 0, layout::kExtent, layout::kExtent}, layout::kBg, 0);
    Label(catalog_overlay_, 24, 25, 300, "全部应用", layout::kText);
    auto* close = Card(catalog_overlay_, {612, 0, 84, 84}, "");
    Icon(close, 25, 25, 34, unsigned(art::Symbol::Close));
    lv_obj_add_event_cb(close, ToggleCatalog, LV_EVENT_SHORT_CLICKED, this);
    catalog_ = lv_obj_create(catalog_overlay_);
    Panel(catalog_, layout::kCatalog, layout::kBg, 0);
    lv_obj_set_flag(catalog_, LV_OBJ_FLAG_SCROLLABLE, true);
    lv_obj_set_scroll_dir(catalog_, LV_DIR_VER);
    Label(catalog_overlay_, 24, 642, 88, "音量", layout::kMuted);
    auto* volume = lv_slider_create(catalog_overlay_);
    lv_obj_set_pos(volume, Scale(120), Scale(642));
    lv_obj_set_size(volume, Scale(264), Scale(12));
    lv_obj_set_ext_click_area(volume, Scale(30));
    lv_slider_set_range(volume, 0, 100);
    lv_slider_set_value(volume, 70, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume, lv_color_hex(layout::kAmber), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume, lv_color_hex(layout::kText), LV_PART_KNOB);
    lv_obj_add_event_cb(volume, ChangeVolume, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_flag(catalog_overlay_, LV_OBJ_FLAG_HIDDEN, true);

    lv_timer_handler();
#if defined(LCD_BACKLIGHT_EN) && LCD_BACKLIGHT_EN
    lcd_backlight_toggle(true);
#endif
    std::puts("MicroPixel Orion Home configured for 480 x 480");
    return true;
}

void OrionHome::Bind(lv_obj_t* object, AppTile& app) {
    lv_obj_add_event_cb(object, Launch, LV_EVENT_SHORT_CLICKED, &app);
}

void OrionHome::AddApp(uint32_t index, const char* title, HomeAppRole role) {
    auto& entry = apps_[index];
    entry = {this, index};
    auto* tile = lv_button_create(catalog_);
    Panel(tile, layout::AppTile(count_apps_++), layout::kSurface, 24);
    auto* badge = lv_obj_create(tile);
    Panel(badge, {16, 24, 52, 52}, layout::kRaised, 26);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    Icon(tile, 25, 33, 34, unsigned(RoleIcon(role)));
    Label(tile, 84, 22, 224, title);
    Label(tile, 84, 56, 224, "点击打开", layout::kMuted);
    Bind(tile, entry);
    lv_label_set_text_fmt(count_, "%u", static_cast<unsigned>(count_apps_));

    switch (role) {
        case HomeAppRole::kClock:
            Bind(clock_card_, entry);
            Bind(alarm_card_, entry);
            Bind(agenda_card_, entry);
            break;
        case HomeAppRole::kWeather:
            Bind(weather_card_, entry);
            Bind(weather_button_, entry);
            break;
        case HomeAppRole::kMusic:
            Bind(music_card_, entry);
            break;
        default:
            break;
    }
}

void OrionHome::Update(const micropixel_wall_time_t& wall, uint64_t next_alarm) {
    const uint64_t minute = wall.valid ? wall.utc / 60 : 0;
    if (last_minute_ != minute || last_offset_ != wall.offset_minutes) {
        last_minute_ = minute;
        last_offset_ = wall.offset_minutes;
        TimeLabel(time_, wall.utc, wall.offset_minutes);
        if (wall.valid) {
            const auto day = calendar::LocalDate(wall.utc, wall.offset_minutes);
            constexpr const char* kWeekday[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
            lv_label_set_text_fmt(date_, "%u月%u日  %s", day.month, day.day, kWeekday[calendar::Weekday(day)]);
            lv_label_set_text_fmt(agenda_date_, "%u月%u日", day.month, day.day);
        } else {
            lv_label_set_text_static(date_, "请同步时间");
            lv_label_set_text_static(agenda_date_, "今天");
        }
        last_alarm_ = UINT64_MAX;
    }
    if (last_alarm_ != next_alarm) {
        last_alarm_ = next_alarm;
        TimeLabel(alarm_, next_alarm, wall.offset_minutes);
        lv_label_set_text_static(alarm_status_, next_alarm ? "下次提醒" : "尚未设置");
    }
}

void OrionHome::UpdateWeather(const WeatherState& state) {
    if (!state.Read(weather_, weather_revision_)) return;
    lv_label_set_text(weather_city_, weather_.title);
    if (weather_.updated_utc) {
        const auto tenths = std::abs(weather_.value % 10);
        if (tenths)
            lv_label_set_text_fmt(weather_value_, "%s%d.%d°", weather_.value < 0 && weather_.value > -10 ? "-" : "",
                                  weather_.value / 10, tenths);
        else
            lv_label_set_text_fmt(weather_value_, "%d°", weather_.value / 10);
    } else {
        lv_label_set_text_static(weather_value_, "--°");
    }
    const char* separator = std::strstr(weather_.subtitle, "·");
    lv_label_set_text(weather_condition_, (weather_.flags & MICROPIXEL_WIDGET_ERROR) ? "刷新失败 · 显示上次天气"
                                          : !weather_.updated_utc ? "等待天气更新"
                                          : separator             ? separator + sizeof("·") - 1
                                                                  : weather_.subtitle);
    const unsigned icon = weather_.updated_utc ? art::Index(weather_.auxiliary) : unsigned(art::Symbol::Unavailable);
    lv_image_set_src(weather_icon_, &icons_->weather[icon].image);
}

void OrionHome::LaunchStatus(const char* text) {
    if (status_) lv_label_set_text_static(status_, text);
}

void OrionHome::UpdateMusic(MediaWidgets& widgets) {
    bool pending{};
    if (!widgets.Read(music_, pending)) return;
    if (music_revision_ == music_.revision && music_online_ == music_.online && music_pending_ == pending) return;
    music_revision_ = music_.revision;
    music_online_ = music_.online;
    music_pending_ = pending;
    const bool has_track = music_.data.value != 0 || music_.data.auxiliary != 0;
    const bool playing = music_online_ && (music_.data.flags & MICROPIXEL_WIDGET_PLAYING);
    const bool active = music_online_ && has_track && !pending;
    lv_obj_set_style_text_font(music_title_, has_track ? &music_font_ : &font_orion_home, 0);
    lv_label_set_text(music_title_, music_.data.title);
    lv_label_set_text_static(music_status_, !music_online_ ? "应用已停止"
                                            : playing      ? "正在播放"
                                            : has_track    ? "已暂停"
                                                           : "选择音乐");
    lv_image_set_src(music_play_icon_, playing ? &icons_->pause.image : &icons_->play.image);
    lv_obj_set_state(music_play_, LV_STATE_DISABLED, !active);
    lv_obj_set_state(music_next_, LV_STATE_DISABLED, !active);
    lv_obj_set_style_bg_color(music_play_background_, lv_color_hex(active ? layout::kAmber : 0x78695c), 0);
    lv_obj_set_style_image_opa(music_play_icon_, active ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_image_opa(music_next_icon_, active ? LV_OPA_COVER : LV_OPA_40, 0);
}

void OrionHome::TogglePlayback(lv_event_t* event) {
    auto& home = *static_cast<OrionHome*>(lv_event_get_user_data(event));
    home.callbacks_.media_action(home.callbacks_.context, home.music_, MICROPIXEL_ACTION_SET_PLAYING,
                                 !(home.music_.data.flags & MICROPIXEL_WIDGET_PLAYING));
}

void OrionHome::NextTrack(lv_event_t* event) {
    auto& home = *static_cast<OrionHome*>(lv_event_get_user_data(event));
    home.callbacks_.media_action(home.callbacks_.context, home.music_, MICROPIXEL_ACTION_NEXT_TRACK, 0);
}

void OrionHome::Launch(lv_event_t* event) {
    auto& tile = *static_cast<AppTile*>(lv_event_get_user_data(event));
    auto& home = *tile.owner;
    lv_obj_set_flag(home.catalog_overlay_, LV_OBJ_FLAG_HIDDEN, true);
    home.callbacks_.launch(home.callbacks_.context, tile.app);
}

void OrionHome::Back(lv_event_t* event) {
    auto& home = *static_cast<OrionHome*>(lv_event_get_user_data(event));
    home.callbacks_.back(home.callbacks_.context);
}

void OrionHome::ToggleCatalog(lv_event_t* event) {
    auto& home = *static_cast<OrionHome*>(lv_event_get_user_data(event));
    lv_obj_set_flag(home.catalog_overlay_, LV_OBJ_FLAG_HIDDEN,
                    !lv_obj_has_flag(home.catalog_overlay_, LV_OBJ_FLAG_HIDDEN));
}

void OrionHome::ChangeVolume(lv_event_t* event) {
    const auto& callbacks = static_cast<OrionHome*>(lv_event_get_user_data(event))->callbacks_;
    callbacks.volume(callbacks.context, static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target_obj(event))));
}

void OrionHome::ReadTouch(lv_indev_t* pointer, lv_indev_data_t* data) {
    auto& home = *static_cast<OrionHome*>(lv_indev_get_user_data(pointer));
    const auto touch = home.touch_.Poll();
    const bool showing_guest = lv_display_get_screen_active(home.display_) == home.guest_;
    if (touch.pressed && !home.front_pressed_) {
        const bool in_guest_content = touch.y < static_cast<int32_t>(GuestPanel::kContentHeight);
        const bool keyboard = home.text_panel_ && !lv_obj_has_flag(home.text_panel_, LV_OBJ_FLAG_HIDDEN);
        home.touch_route_ = !showing_guest      ? TouchRoute::kHome
                            : !in_guest_content ? TouchRoute::kNavigation
                            : keyboard          ? TouchRoute::kKeyboard
                                                : TouchRoute::kGuest;
        home.home_swipe_ = home.touch_route_ == TouchRoute::kNavigation && touch.y >= LCD_H - 22 &&
                           touch.x >= LCD_W / 2 - 54 && touch.x < LCD_W / 2 + 54;
    }
    const bool host_pressed =
        touch.pressed && (home.touch_route_ == TouchRoute::kKeyboard ||
                          home.touch_route_ == (showing_guest ? TouchRoute::kNavigation : TouchRoute::kHome));
    if (showing_guest && home.touch_route_ == TouchRoute::kGuest && (touch.pressed || home.front_pressed_)) {
        const int32_t y = std::min(touch.y, static_cast<int32_t>(GuestPanel::kContentHeight - 1));
        home.callbacks_.touch(home.callbacks_.context, touch.x, y, touch.pressed);
    } else if (showing_guest && home.home_swipe_ && touch.pressed && touch.y < LCD_H - 68) {
        home.home_swipe_ = false;
        lv_indev_wait_release(pointer);
        home.callbacks_.home(home.callbacks_.context);
    }
    if (!touch.pressed) home.home_swipe_ = false;
    home.front_pressed_ = touch.pressed;
    data->point = {touch.x, touch.y};
    data->state = host_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void OrionHome::KeyboardAction(lv_event_t* event) {
    auto& home = *static_cast<OrionHome*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
        home.callbacks_.back(home.callbacks_.context);
        return;
    }
    const char* text = lv_textarea_get_text(home.text_area_);
    if (home.callbacks_.submit_text(home.callbacks_.context, text)) home.ShowKeyboard(false);
}

void OrionHome::ShowKeyboard(bool visible) {
    if (!visible) {
        if (text_panel_) lv_obj_add_flag(text_panel_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!text_panel_) {
        text_panel_ = lv_obj_create(guest_);
        const int height = 242;
        Panel(text_panel_,
              {0, DesignPixels(GuestPanel::kContentHeight - height), layout::kExtent, DesignPixels(height)},
              layout::kSurface, 0);
        text_area_ = lv_textarea_create(text_panel_);
        lv_obj_set_pos(text_area_, 8, 4);
        lv_obj_set_size(text_area_, LCD_W - 16, 41);
        lv_textarea_set_one_line(text_area_, true);
        lv_textarea_set_max_length(text_area_, 128);
        lv_obj_set_style_text_font(text_area_, &font_orion_home, 0);
        auto* keyboard = lv_keyboard_create(text_panel_);
        lv_obj_set_size(keyboard, LCD_W, height - 49);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(keyboard, text_area_);
        lv_obj_add_event_cb(text_area_, KeyboardAction, LV_EVENT_READY, this);
        lv_obj_add_event_cb(text_area_, KeyboardAction, LV_EVENT_CANCEL, this);
        lv_obj_add_flag(text_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_has_flag(text_panel_, LV_OBJ_FLAG_HIDDEN)) {
        lv_textarea_set_text(text_area_, "");
        lv_obj_move_foreground(text_panel_);
        lv_obj_remove_flag(text_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* OrionHome::GuestSurface(bool visible) {
    auto* requested = visible ? guest_ : home_;
    if (lv_display_get_screen_active(display_) != requested) lv_screen_load(requested);
    return visible ? guest_ : nullptr;
}

uint32_t OrionHome::Frames() const { return scanout_frames.load(std::memory_order_relaxed); }

// Keep the UART encoding buffer off the LVGL render path under LTO.
[[gnu::noinline]] bool OrionHome::CaptureFrame() const {
    auto* uart = bflb_device_get_by_name("uart0");
    auto* framebuffer = lcd_get_screen_using();
    if (!uart || !framebuffer || !Frames()) return false;
    constexpr uint32_t kBytes = LCD_W * LCD_H * sizeof(lcd_color_t);
    constexpr uint32_t kChunkBytes = 256;
    static_assert(kBytes % kChunkBytes == 0);
    constexpr char kHex[] = "0123456789abcdef";
    char line[17 + 2 * kChunkBytes + 1];
    auto send = [uart, &line](uint32_t length) {
        return bflb_uart_put_block(uart, reinterpret_cast<uint8_t*>(line), length) == 0;
    };
    const auto crc = bflb_soft_crc32(framebuffer, kBytes);
    const int header = std::snprintf(line, sizeof(line), "\nMPXFB RGB565LE %u %u %08lx\n", LCD_W, LCD_H,
                                     static_cast<unsigned long>(crc));
    if (header <= 0 || header >= static_cast<int>(sizeof(line)) || !send(header)) return false;
    const auto* pixels = reinterpret_cast<const uint8_t*>(framebuffer);
    for (uint32_t offset = 0; offset < kBytes; offset += kChunkBytes) {
        const int prefix =
            std::snprintf(line, sizeof(line), "MPXFB %04lx ", static_cast<unsigned long>(offset / kChunkBytes));
        if (prefix != 11) return false;
        auto* hex = line + prefix;
        for (uint32_t i = 0; i < kChunkBytes; ++i) {
            const auto byte = pixels[offset + i];
            hex[2 * i] = kHex[byte >> 4];
            hex[2 * i + 1] = kHex[byte & 15];
        }
        hex[2 * kChunkBytes] = '\n';
        if (!send(prefix + 2 * kChunkBytes + 1)) return false;
        if ((offset / kChunkBytes) % 16 == 15) vTaskDelay(1);
    }
    return true;
}

void OrionHome::Refresh() { lv_timer_handler(); }

}  // namespace micropixel::bouffalo
