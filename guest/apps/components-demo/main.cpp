// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <optional>

#include "sdk/micropixel.hpp"

namespace {
using namespace micropixel;
using namespace micropixel::literals;
void Log(const Application& app, const char* name, uint32_t value) {
    FixedString<96> text;
    text.Append(name);
    text.AppendUint(value);
    app.log().Info(text.c_str());
}
struct Model {
    uint32_t color{}, creates{}, service_ticks{}, first_token{};
    bool self_test{}, destroyed{}, recreated{}, stop_requested{};
};
class DemoActivity final : public Activity {
   public:
    explicit DemoActivity(Model& model) : model_(model) {}
    void OnCreate(ComponentContext& context) override {
        const auto& app = context.app();
        scene_.reset();
        texture_.Reset();
        if (!model_.color) model_.color = (app.random().U32() & 0x7f7f7f) | 0x404040;
        if (!model_.first_token) model_.first_token = context.token();
        if (!model_.creates)
            app.renderer()
                .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
                .value();
        std::array<uint8_t, 16> pixels{};
        for (unsigned i = 0; i < 4; ++i) {
            pixels[i * 4] = model_.color & 255;
            pixels[i * 4 + 1] = (model_.color >> 8) & 255;
            pixels[i * 4 + 2] = (model_.color >> 16) & 255;
            pixels[i * 4 + 3] = 255;
        }
        texture_ = app.resources().CreateDynamicTexture({2, 2}, PixelFormat::kBgra8888, pixels, 8).value();
        scene_.emplace(app.renderer().CreateScene(Color::Rgb(242, 245, 249)).value());
        (void)scene_->CreateLabel({24, 28}, "Activity + Service", Color::Black(), SystemFont::kTitle).value();
        (void)scene_->CreateLabel({24, 80}, "Home/F1 keeps the background service running", Color::Black()).value();
        (void)scene_->CreateSprite(texture_, {40, 130, 640, 300}, {0, 0, 2, 2}).value();
        const char* labels[]{"Destroy Activity", "Start/Stop Service", "Exit App"};
        for (int i = 0; i < 3; ++i) {
            (void)scene_->CreateShape({24 + i * 232, 520, 216, 80}, Color::White()).value();
            (void)scene_->CreateLabel({32 + i * 232, 545}, labels[i], Color::Black(), SystemFont::kSmall).value();
        }
        timer_.emplace(app.timers().Every(100_ms).value());
        app.renderer().Present(*scene_).value();
        Log(app, "COMP create=", ++model_.creates);
        Log(app, "COMP activity token=", context.token());
        Log(app, "COMP color=", model_.color);
    }
    void OnResume(ComponentContext& context) override {
        context.app().input().EnableText(true).value();
        context.app().log().Info("COMP resumed");
        if (scene_) context.app().renderer().Present(*scene_).value();
    }
    void OnStop(ComponentContext& context) override {
        context.app().input().EnableText(false).value();
        context.app().log().Info("COMP stopped");
    }
    void OnDestroy(ComponentContext& context) override {
        // Intentionally leave the timer / texture to exercise Host domain cleanup.
        context.app().log().Info("COMP destroyed");
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if ((timer_ && event.TimerFrom(*timer_))) {
            auto status = context.Status().value();
            if (model_.self_test && model_.stop_requested && status.service_state == 0 && model_.creates >= 2) {
                Assert(status.activity_token != model_.first_token, "activity epoch was reused");
                app.log().Info("COMPONENTS PASS independent lifecycle, resource cleanup and new activity generation");
                context.Exit().value();
                return;
            }
            auto text = app.input().ReadText();
            if (text && !text->empty()) app.log().Info("COMP text received");
            if (scene_) app.renderer().Present(*scene_).value();
        }
        if (event.key() && event.key()->phase() == KeyPhase::kDown) app.log().Info("COMP key received");
        const auto* touch = event.touch();
        if (!touch || touch->phase() != TouchPhase::kUp || touch->y() < 520 || touch->y() >= 600) return;
        if (touch->x() < 240)
            context.DestroyActivity().value();
        else if (touch->x() < 480) {
            const auto info = context.Status().value();
            if (info.service_state == 2)
                context.StopService().value();
            else
                context.StartService().value();
        } else
            context.Exit().value();
    }

   private:
    Model& model_;
    std::optional<Scene> scene_;
    Texture texture_;
    std::optional<Timer> timer_;
};
class DemoService final : public AppService {
   public:
    explicit DemoService(Model& model) : model_(model) {}
    void OnStart(ComponentContext& context) override {
        const auto& app = context.app();
        timer_.emplace(app.timers().Every(10_ms).value());
        stream_ = app.audio()
                      .OpenPcmStream({.sample_rate = 48000,
                                      .channels = 1,
                                      .capacity_frames = 8192,
                                      .low_water_frames = 0,
                                      .volume_per_mille = 300})
                      .value();
        if (!model_.service_ticks)
            if (const auto* url = app.launch_arguments().FindValue("--test-url"))
                http_ = app.network().Start(HttpMethod::kGet, url).value();
        Log(app, "COMP service started token=", context.token());
        Feed();
    }
    void OnStop(ComponentContext& context) override {
        // Host closes the component's timer, request and PCM even without Reset().
        context.app().log().Info("COMP service stopped");
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        if (!(timer_ && event.TimerFrom(*timer_))) return;
        const auto& app = context.app();
        Feed();
        ++model_.service_ticks;
        if (model_.service_ticks % 25 == 0) Log(app, "COMP heartbeat=", model_.service_ticks);
        if (http_.valid()) {
            const auto state = http_.State().value();
            if (state.phase == HttpPhase::kComplete) {
                Log(app, "COMP HTTP bytes=", state.body_length);
                http_.Close();
            }
        }
        if (model_.self_test) {
            const auto info = context.Status().value();
            if (model_.service_ticks >= 50 && !model_.destroyed) {
                model_.destroyed = true;
                context.DestroyActivity().value();
            } else if (model_.destroyed && info.activity_state == 0 && !model_.recreated) {
                Assert(info.service_state == 2 && info.resource_count == 2,
                       "activity cleanup affected service resources");
                model_.recreated = true;
                app.log().Info("COMP service survived activity destroy");
                context.CreateActivity().value();
            } else if (model_.recreated && model_.creates >= 2 && !model_.stop_requested) {
                model_.stop_requested = true;
                context.StopService().value();
            }
        }
    }

   private:
    void Feed() {
        for (unsigned batch = 0; batch < 16; ++batch) {
            for (auto& sample : samples_) sample = ((phase_++ / 16) & 1) ? 3000 : -3000;
            const auto result = stream_.Write(samples_.data(), samples_.size());
            Assert(result.has_value(), "service PCM write");
            if (*result < samples_.size()) break;
        }
    }
    Model& model_;
    std::optional<Timer> timer_;
    PcmStream stream_;
    HttpRequest http_;
    uint32_t phase_{};
    std::array<int16_t, 512> samples_{};
};
}  // namespace
int main() {
    Application app;
    Model model;
    model.self_test = app.launch_arguments().HasFlag("--self-test");
    DemoActivity activity(model);
    DemoService service(model);
    app.RunComponents(&activity, &service);
    return 0;
}
