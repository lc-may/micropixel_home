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
        model_.local_refresh = !now;
        if (now && now->valid) {
            auto registered = app.jobs().Register({.deadline_ms = Budget(), .next_utc = now->utc});
            if (!registered) app.log().Info("WEATHER job registration failed");
        }
        app.log().Info("WEATHER service ready");
    }
    void OnStop(ComponentContext&) override {
        client_.Cancel();
        timer_.reset();
        if (model_.local_refresh) {
            model_.busy = false;
            next_attempt_ = 0;
        }
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
        if (timer_ && event.TimerFrom(*timer_)) {
            if (model_.local_refresh)
                RefreshLocally();
            else if (active_) {
                const int result = Poll();
                if (result) Finish(result > 0);
            }
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
    bool StartClient() {
        active_city_ = model_.saved.city;
        if (!client_.Start(*app_, active_city_)) return false;
        model_.busy = true;
        ++model_.revision;
        app_->log().Info("WEATHER refresh started");
        return true;
    }
    int Poll() {
        Reading reading{};
        const int result = client_.Poll(reading);
        if (result <= 0) return result;
        if (active_city_ != model_.saved.city) return -1;
        model_.saved.reading = reading;
        model_.Save(*app_);
        return 1;
    }
    void RefreshLocally() {
        constexpr uint64_t kRetryUs = 5000000;
        constexpr uint64_t kRefreshIntervalUs = 15 * 60 * 1000000;
        const auto now = app_->clock().Now().microseconds();
        if (refresh_requests_ != model_.refresh_requests) {
            refresh_requests_ = model_.refresh_requests;
            client_.Cancel();
            model_.busy = false;
            next_attempt_ = now;
        }
        if (model_.busy) {
            const int result = Poll();
            if (!result && now < deadline_) return;
            FinishClient(result > 0);
            next_attempt_ = now + (result > 0 ? kRefreshIntervalUs : kRetryUs);
        } else if (now >= next_attempt_) {
            if (StartClient())
                deadline_ = now + 1000ULL * Budget();
            else {
                if (!model_.failed) FinishClient(false);
                next_attempt_ = now + kRetryUs;
            }
        }
    }
    void Start(uint32_t ticket) {
        active_ = ticket;
        auto entered = app_->jobs().Enter(ticket);
        if (!entered) {
            (void)app_->jobs().Complete(ticket);
            active_ = 0;
            return;
        }
        if (!StartClient()) Finish(false);
    }
    void FinishClient(bool success) {
        client_.Cancel();
        model_.busy = false;
        model_.failed = !success;
        model_.Publish(*app_);
        app_->log().Info(success ? "WEATHER refresh success" : "WEATHER refresh failed; previous value retained");
    }
    void Finish(bool success) {
        FinishClient(success);
        (void)app_->jobs().Complete(active_);
        active_ = 0;
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
    uint32_t active_{}, queued_{}, message_{}, active_city_{}, refresh_requests_{};
    uint64_t next_attempt_{}, deadline_{};
};
}  // namespace weather
