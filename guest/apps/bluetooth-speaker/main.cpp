// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "sdk/bluetooth.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/icon_button.hpp"
#include "sdk/ui/wrap_text.hpp"

namespace bluetooth_speaker {
namespace {
namespace mp = micropixel;
namespace ui = mp::ui;
constexpr mp::Size kCanvas{480, 436};
constexpr auto kBackground = mp::Color::Rgb(24, 18, 20);
constexpr auto kSurface = mp::Color::Rgb(42, 34, 38);
constexpr auto kGroove = mp::Color::Rgb(55, 45, 49);
constexpr auto kDisc = mp::Color::Rgb(30, 26, 28);
constexpr auto kWhite = mp::Color::Rgb(252, 242, 243);
constexpr auto kMuted = mp::Color::Rgb(186, 163, 169);
constexpr auto kRed = mp::Color::Rgb(208, 42, 64);
constexpr auto kPink = mp::Color::Rgb(255, 121, 137);
constexpr mp::Rect kProgressBar{64, 310, 352, 4};

struct Playback final {
    mp::Bluetooth bluetooth;
    micropixel_bluetooth_status_t audio{};
    micropixel_bluetooth_remote_status_t remote{};
    micropixel_bluetooth_remote_track_t track{};
    int audio_result{MICROPIXEL_STATUS_UNSUPPORTED};
    int remote_result{MICROPIXEL_STATUS_UNSUPPORTED};
    int key_result{};
    uint32_t revision{1};

    void Refresh() {
        micropixel_bluetooth_status_t next_audio{};
        micropixel_bluetooth_remote_status_t next_remote{};
        micropixel_bluetooth_remote_track_t next_track{};
        const int next_audio_result = bluetooth.Status(next_audio);
        const int next_remote_result = next_audio_result == MICROPIXEL_STATUS_OK ? bluetooth.RemoteStatus(next_remote)
                                                                                 : MICROPIXEL_STATUS_UNSUPPORTED;
        if (next_remote_result == MICROPIXEL_STATUS_OK && next_remote.link == MPX_BT_REMOTE_READY) {
            if (bluetooth.RemoteTrack(next_track) != MICROPIXEL_STATUS_OK || next_track.session != next_remote.session)
                next_track = {};
        }
        if (next_audio_result == audio_result && next_remote_result == remote_result &&
            next_audio.revision == audio.revision && std::memcmp(&next_remote, &remote, sizeof(remote)) == 0 &&
            std::memcmp(&next_track, &track, sizeof(track)) == 0)
            return;
        if (next_remote.session != remote.session) key_result = 0;
        audio = next_audio;
        remote = next_remote;
        track = next_track;
        audio_result = next_audio_result;
        remote_result = next_remote_result;
        ++revision;
    }

    void Send(uint32_t key) {
        key_result = bluetooth.RemoteKey(remote.session, key);
        Refresh();
        ++revision;
    }

    bool playing() const {
        if (remote.playback != MPX_BT_REMOTE_UNKNOWN) return remote.playback == MPX_BT_REMOTE_PLAYING;
        return audio.stream == MPX_BT_STREAM_PLAYING;
    }
};

class PlaybackService final : public mp::AppService {
   public:
    explicit PlaybackService(Playback& playback) : playback_(playback) {}

    void OnStart(mp::ComponentContext& context) override {
        context.app().log().Info("BT_SPEAKER service started");
        if (playback_.bluetooth.Open() != MICROPIXEL_STATUS_OK) return;
        playback_.Refresh();
        if (playback_.audio_result == MICROPIXEL_STATUS_OK)
            timer_.emplace(context.app().timers().Every(mp::Duration::Milliseconds(400)).value());
    }

    void OnEvent(mp::ComponentContext&, const mp::Event& event) override {
        if (timer_ && event.TimerFrom(*timer_)) playback_.Refresh();
    }

    void OnStop(mp::ComponentContext& context) override {
        timer_.reset();
        context.app().log().Info("BT_SPEAKER service stopped");
    }

   private:
    Playback& playback_;
    std::optional<mp::Timer> timer_;
};

void AppendTime(mp::FixedString<40>& text, uint32_t milliseconds) {
    const uint32_t seconds = milliseconds / 1000;
    text.AppendUint(seconds / 60);
    text.Append(seconds % 60 < 10 ? ":0" : ":");
    text.AppendUint(seconds % 60);
}

class PlayerView final {
   public:
    explicit PlayerView(const mp::Application& app)
        : scene(app.renderer().CreateScene(kBackground).value()), renderer_(app.renderer()) {
        (void)scene.CreateLabel({24, 16}, "蓝牙音箱", kWhite, mp::SystemFont::kLarge).value();
        (void)scene.CreateLabel({428, 23}, "A2DP", kMuted, mp::SystemFont::kSmall, true).value();
        auto circle = [&](int32_t diameter, mp::Color fill) {
            return scene
                .CreateRoundedRect({(480 - diameter) / 2, 136 - diameter / 2, diameter, diameter},
                                   {.fill = fill, .radius = static_cast<uint32_t>(diameter / 2)})
                .value();
        };
        (void)circle(188, kGroove);
        (void)circle(184, kDisc);
        (void)circle(152, kGroove);
        (void)circle(150, kDisc);
        (void)circle(116, kGroove);
        (void)circle(114, kDisc);
        disc_label_ = circle(76, kSurface);
        (void)scene.CreateLabel({240, 119}, mp::symbols::kBluetooth, kWhite, mp::SystemFont::kLarge, true).value();
        state_ = scene.CreateLabel({240, 234}, "等待音源连接", kWhite, mp::SystemFont::kLarge, true).value();
        source_ = scene.CreateLabel({240, 267}, " ", kMuted, mp::SystemFont::kSmall, true).value();
        album_ = scene.CreateLabel({240, 289}, " ", kMuted, mp::SystemFont::kSmall, true).value();
        bar_ = scene.CreateShape(kProgressBar, kGroove).value();
        progress_ = scene.CreateShape(kProgressBar, kPink).value();
        time_ = scene.CreateLabel({240, 318}, " ", kMuted, mp::SystemFont::kSmall, true).value();
        hint_ = scene.CreateLabel({240, 410}, " ", kMuted, mp::SystemFont::kSmall, true).value();
        const auto button = [&](mp::Rect bounds, const char* icon, mp::Color fill) {
            return scene.CreateIconButton({.bounds = bounds,
                                           .icon = icon,
                                           .style = {.background = fill,
                                                     .text = kWhite,
                                                     .feedback = kBackground,
                                                     .font = mp::SystemFont::kLarge,
                                                     .corner_radius = static_cast<uint32_t>(bounds.height / 2)},
                                           .enabled = false});
        };
        previous = button({113, 347, 56, 54}, mp::symbols::kPrevious, kSurface);
        toggle = button({208, 342, 64, 64}, mp::symbols::kPlay, kRed);
        next = button({311, 347, 56, 54}, mp::symbols::kNext, kSurface);
    }

    void Update(const Playback& model) {
        const auto& audio = model.audio;
        const auto& remote = model.remote;
        const bool playing = model.playing();
        const bool connected =
            model.audio_result == MICROPIXEL_STATUS_OK && audio.state == MPX_BT_CONNECTED && audio.role == MPX_BT_SINK;
        const bool ready =
            connected && model.remote_result == MICROPIXEL_STATUS_OK && remote.link == MPX_BT_REMOTE_READY;
        const char* state = "等待音源连接";
        const char* hint = "在「配置 / 经典蓝牙」开启可发现";
        if (model.audio_result != MICROPIXEL_STATUS_OK || audio.state == MPX_BT_UNAVAILABLE) {
            state = "蓝牙服务不可用";
            hint = "需要支持蓝牙音箱的 Orion Host";
        } else if (audio.role == MPX_BT_SOURCE) {
            state = "当前为蓝牙发送模式";
            hint = "请在「配置 / 经典蓝牙」切换到接收";
        } else if (connected) {
            state = playing ? "正在播放"
                    : audio.stream == MPX_BT_STREAM_SUSPENDED || remote.playback == MPX_BT_REMOTE_PAUSED ? "已暂停"
                                                                                                         : "已连接";
            if (model.remote_result != MICROPIXEL_STATUS_OK || remote.link == MPX_BT_REMOTE_UNAVAILABLE)
                hint = "遥控不可用，请从音源端操作";
            else if (!ready)
                hint = "请从音源端开始播放，等待遥控连接";
            else if (remote.pending)
                hint = "正在发送播放指令";
            else if (remote.error || model.key_result)
                hint = "播放指令发送失败，请重试";
            else
                hint = "返回桌面后继续播放";
        }
        if (std::memcmp(&displayed_track_, &model.track, sizeof(displayed_track_)) != 0) {
            displayed_track_ = model.track;
            track_lines_[0] = Fit(displayed_track_.title, mp::SystemFont::kLarge);
            track_lines_[1] = Fit(displayed_track_.artist, mp::SystemFont::kSmall);
            track_lines_[2] = Fit(displayed_track_.album, mp::SystemFont::kSmall);
        }
        const bool has_track = !track_lines_[0].empty() || !track_lines_[1].empty() || !track_lines_[2].empty();
        state_.SetText(track_lines_[0].empty() ? state : track_lines_[0].c_str());
        hint_.SetText(hint);
        mp::FixedString<64> source;
        if (connected) {
            source.Append(audio.peer_address);
            if (audio.sample_rate) {
                source.Append("  ·  SBC  ");
                source.AppendUint(audio.sample_rate);
                source.Append(" Hz");
            }
        } else {
            source.Append("在手机或电脑上选择 MicroPixel Orion");
        }
        source_.SetText(has_track ? (track_lines_[1].empty() ? " " : track_lines_[1].c_str()) : source.c_str());
        album_.SetVisible(!track_lines_[2].empty());
        if (!track_lines_[2].empty()) album_.SetText(track_lines_[2].c_str());
        disc_label_.SetFillColor(playing ? kRed : kSurface);
        toggle.SetIcon(playing ? mp::symbols::kPause : mp::symbols::kPlay).value();
        for (auto* button : {&previous, &toggle, &next}) button->SetEnabled(ready && !remote.pending);

        const bool has_time = ready && remote.position_ms != MPX_BT_REMOTE_UNKNOWN_TIME &&
                              remote.duration_ms != MPX_BT_REMOTE_UNKNOWN_TIME && remote.duration_ms != 0;
        bar_.SetVisible(has_time);
        time_.SetVisible(has_time);
        progress_.SetVisible(has_time && remote.position_ms != 0);
        if (has_time) {
            const auto position = std::min(remote.position_ms, remote.duration_ms);
            auto fill = kProgressBar;
            fill.width = std::max(1, static_cast<int32_t>(uint64_t(fill.width) * position / remote.duration_ms));
            progress_.SetRect(fill);
            mp::FixedString<40> time;
            AppendTime(time, position);
            time.Append("  /  ");
            AppendTime(time, remote.duration_ms);
            time_.SetText(time.c_str());
        }
    }

    mp::Scene scene;
    ui::IconButton previous, toggle, next;

   private:
    std::string Fit(const char* text, mp::SystemFont font) const {
        if (!*text) return {};
        return ui::WrapText<1>(text, 432, [&](std::string_view value) {
            const std::string terminated{value};
            const auto metrics = renderer_.MeasureText(terminated.c_str(), font);
            return metrics ? metrics->width : UINT32_MAX;
        })[0];
    }

    mp::Renderer renderer_;
    micropixel_bluetooth_remote_track_t displayed_track_{};
    std::array<std::string, 3> track_lines_;
    mp::RoundedRectNode disc_label_;
    mp::ShapeNode bar_, progress_;
    mp::LabelNode state_, source_, album_, time_, hint_;
};

class PlayerActivity final : public mp::Activity {
   public:
    explicit PlayerActivity(Playback& playback) : playback_(playback) {}

    void OnCreate(mp::ComponentContext& context) override {
        view_ = std::make_unique<PlayerView>(context.app());
        Flush(context.app());
    }

    void OnResume(mp::ComponentContext& context) override {
        timer_.emplace(context.app().timers().Every(mp::Duration::Milliseconds(200)).value());
        Flush(context.app());
        context.app().log().Info("BT_SPEAKER activity visible");
    }

    void OnStop(mp::ComponentContext& context) override {
        timer_.reset();
        for (auto* button : {&view_->previous, &view_->toggle, &view_->next}) button->Reset();
        context.app().log().Info("BT_SPEAKER activity hidden");
    }

    void OnDestroy(mp::ComponentContext& context) override {
        view_.reset();
        context.app().log().Info("BT_SPEAKER activity closed");
    }

    bool OnNavigateBack(mp::ComponentContext& context) override {
        context.DestroyActivity().value();
        return true;
    }

    void OnEvent(mp::ComponentContext& context, const mp::Event& event) override {
        if (!timer_) return;
        if (event.TimerFrom(*timer_) && seen_ != playback_.revision) Flush(context.app());
        if (const auto* touch = event.touch()) {
            const std::array buttons{&view_->previous, &view_->toggle, &view_->next};
            const std::array<uint32_t, 3> keys{MPX_BT_REMOTE_PREVIOUS,
                                               playback_.playing() ? MPX_BT_REMOTE_PAUSE : MPX_BT_REMOTE_PLAY,
                                               MPX_BT_REMOTE_NEXT};
            bool redraw = false;
            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto update = buttons[i]->OnTouch(*touch);
                if (update.clicked) playback_.Send(keys[i]);
                redraw |= update.redraw();
            }
            if (redraw) Flush(context.app());
        }
    }

   private:
    void Flush(const mp::Application& app) {
        view_->Update(playback_);
        seen_ = playback_.revision;
        app.renderer().Present(view_->scene).value();
    }

    Playback& playback_;
    std::unique_ptr<PlayerView> view_;
    std::optional<mp::Timer> timer_;
    uint32_t seen_{};
};
}  // namespace
}  // namespace bluetooth_speaker

int main() {
    micropixel::Application app;
    app.renderer()
        .ConfigureDisplay(
            {.logical_size = bluetooth_speaker::kCanvas, .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
        .value();
    bluetooth_speaker::Playback playback;
    bluetooth_speaker::PlaybackService service(playback);
    bluetooth_speaker::PlayerActivity activity(playback);
    app.RunComponents(&activity, &service);
    return 0;
}
