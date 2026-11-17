// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "weather_model.hpp"
namespace weather {
class WeatherService final : public AppService {
   public:
    explicit WeatherService(Model& model) : model_(model) {}
    void OnStart(ComponentContext& context) override {
        auto& app = context.app();
        app_ = &app;
        timer_.emplace(app.timers().Every(Duration::Milliseconds(100)).value());
        model_.Publish(app);
        auto now = app.jobs().Now();
        if (now && now->valid) {
            auto registered = app.jobs().Register({.deadline_ms = Budget(), .next_utc = now->utc});
            if (!registered) app.log().Info("WEATHER job registration failed");
        }
        app.log().Info("WEATHER service ready");
    }
    void OnStop(ComponentContext&) override {
        client_.Cancel();
        timer_.reset();
    }
    void OnEvent(ComponentContext&, const Event& event) override {
        if (auto* job = event.job()) {
            if (job->cancelled()) {
                if (job->ticket == active_) {
                    client_.Cancel();
                    Finish(false);
                } else {
                    if (queued_ == job->ticket) queued_ = 0;
                    (void)app_->jobs().Complete(job->ticket);
                }
                return;
            }
            if (active_) {
                queued_ = job->ticket;
                return;
            }
            Start(job->ticket);
            return;
        }
        if (auto* incoming = event.message()) {
            auto m = app_->messages().Read(incoming->ticket);
            if (!m) return;
            if (m->type != static_cast<uint16_t>(AppAction::kRefresh) || message_) {
                (void)app_->messages().Ack(incoming->ticket, -4);
                return;
            }
            auto now = app_->jobs().Now();
            if (!now || !now->valid) {
                (void)app_->messages().Ack(incoming->ticket, -10);
                return;
            }
            auto registered =
                app_->jobs().Register({.id = 2, .interval_seconds = 0, .deadline_ms = Budget(), .next_utc = now->utc});
            if (!registered) {
                (void)app_->messages().Ack(incoming->ticket, -5);
                return;
            }
            message_ = incoming->ticket;
            return;
        }
        if (timer_ && event.TimerFrom(*timer_) && active_) {
            Reading reading{};
            int result = client_.Poll(reading);
            if (!result) return;
            if (result > 0 && active_city_ != model_.saved.city) result = -1;
            if (result > 0) {
                model_.saved.reading = reading;
                model_.Save(*app_);
            }
            Finish(result > 0);
        }
    }

   private:
    uint32_t Budget() const {
        const char* value = app_->launch_arguments().FindValue("--weather-budget-ms");
        if (value) {
            uint32_t n = 0;
            for (; *value >= '0' && *value <= '9'; ++value) n = n * 10 + *value - '0';
            if (n >= 1000 && n <= 60000) return n;
        }
        return 20000;
    }
    void Start(uint32_t ticket) {
        active_ = ticket;
        active_city_ = model_.saved.city;
        auto entered = app_->jobs().Enter(ticket);
        if (!entered) {
            (void)app_->jobs().Complete(ticket);
            active_ = 0;
            return;
        }
        model_.busy = true;
        ++model_.revision;
        if (!client_.Start(*app_, model_.saved.city))
            Finish(false);
        else
            app_->log().Info("WEATHER refresh started");
    }
    void Finish(bool success) {
        client_.Cancel();
        model_.busy = false;
        model_.failed = !success;
        model_.Publish(*app_);
        (void)app_->jobs().Complete(active_);
        active_ = 0;
        app_->log().Info(success ? "WEATHER refresh success" : "WEATHER refresh failed; previous value retained");
        if (message_) {
            (void)app_->messages().Ack(message_, success ? 0 : -5);
            message_ = 0;
        }
        if (queued_) {
            auto next = queued_;
            queued_ = 0;
            Start(next);
        }
    }
    Model& model_;
    const Application* app_{};
    WeatherClient client_;
    std::optional<Timer> timer_;
    uint32_t active_{}, queued_{}, message_{}, active_city_{};
};
}  // namespace weather
