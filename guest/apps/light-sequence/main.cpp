// SPDX-License-Identifier: Apache-2.0
#include "panel.hpp"

namespace light_sequence {
namespace {
class LightActivity final : public mp::Activity {
   private:
    Screen& Current() { return pages_[depth_]; }

    void OnCreate(mp::ComponentContext& context) override {
        const auto& app = context.app();
        model_.Load(app);
        depth_ = 0;
        pages_[0] = {};
        panel_ = std::make_unique<Panel>(app);
        Draw();
        panel_->Present();
    }

    void OnResume(mp::ComponentContext& context) override {
        visible_ = true;
        if (save_pending_) Persist(context.app());
        panel_->Present();
    }

    void OnStop(mp::ComponentContext& context) override {
        visible_ = false;
        toast_.reset();
        panel_->ClearToast();
        panel_->Cancel(Current(), model_);
        save_retry_.reset();
        if (save_pending_) Persist(context.app());
    }

    void OnDestroy(mp::ComponentContext&) override {
        visible_ = false;
        toast_.reset();
        save_retry_.reset();
        save_pending_ = false;
        panel_.reset();
    }

    bool OnNavigateBack(mp::ComponentContext&) override {
        if (!Back()) return false;
        panel_->Present();
        return true;
    }

    void OnEvent(mp::ComponentContext& context, const mp::Event& event) override {
        if (!visible_) return;
        if (save_retry_ && event.TimerFrom(*save_retry_)) {
            save_retry_.reset();
            Persist(context.app());
            return;
        }
        if (toast_ && event.TimerFrom(*toast_)) {
            toast_.reset();
            if (panel_->ClearToast()) panel_->Present();
            return;
        }
        const auto* touch = event.touch();
        if (!touch) return;
        bool commit = false;
        const auto update = panel_->Touch(*touch, Current(), model_, commit);
        if (commit) Persist(context.app());
        const bool changed = update.action && Apply(context.app(), *update.action);
        if (changed || update.changed) panel_->Present();
    }

    void Draw() {
        toast_.reset();
        panel_->Draw(Current(), model_);
    }

    void Open(Screen screen) {
        mp::Assert(depth_ + 1 < pages_.size(), "light-sequence screen depth");
        Current().scroll = panel_->offset();
        pages_[++depth_] = screen;
        Draw();
    }

    bool Back() {
        if (depth_ == 0) return false;
        panel_->Cancel(Current(), model_);
        --depth_;
        Draw();
        return true;
    }

    void Persist(const mp::Application& app) {
        save_pending_ = panel_->adjusting();
        if (!save_pending_) {
            const auto result = model_.Save(app);
            save_pending_ = !result && result.error().code() == mp::ErrorCode::kRateLimited;
        }
        if (!save_pending_)
            save_retry_.reset();
        else if (visible_ && !save_retry_) {
            auto timer = app.timers().After(mp::Duration::Milliseconds(1000));
            if (timer) save_retry_.emplace(std::move(*timer));
        }
    }

    void Batch(const mp::Application& app, std::span<const int> ids, bool on) {
        const auto counts = model_.Count(ids);
        const int changed = model_.SetBatch(ids, on);
        if (changed) Persist(app);
        Draw();
        Copy message;
        if (changed) {
            message.Append(on ? "已开 " : "已关 ");
            message.AppendInt(changed);
            message.Append(" 盏灯");
        } else
            message.Append(on ? "在线灯已全部开启" : "在线灯已全部关闭");
        if (counts.offline) {
            message.Append(" · 跳过 ");
            message.AppendInt(counts.offline);
            message.Append(" 盏离线");
        }
        auto timer = app.timers().After(mp::Duration::Milliseconds(2200));
        if (timer) {
            toast_.emplace(std::move(*timer));
            panel_->Notify(message.c_str());
        }
    }

    bool Apply(const mp::Application& app, Action action) {
        auto& screen = Current();
        switch (action.command) {
            case Command::kBack:
                return Back();
            case Command::kRoom:
                Open({.page = Page::kRoom, .room = action.id});
                return true;
            case Command::kGroup:
                Open({.page = Page::kGroup, .room = screen.room, .id = action.id});
                return true;
            case Command::kLight:
                Open({.page = Page::kLight, .room = screen.room, .id = action.id});
                return true;
            case Command::kRoomTab:
                if (screen.groups == action.target) return false;
                screen.groups = action.target;
                screen.scroll = 0;
                Draw();
                return true;
            case Command::kBatchRoom:
                Batch(app, kRooms[action.id].lights, action.target);
                return true;
            case Command::kBatchGroup:
                Batch(app, kGroups[action.id].members, action.target);
                return true;
            case Command::kToggle:
                model_[action.id].on = !model_[action.id].on;
                break;
            case Command::kMode:
                if (model_[screen.id].rgb == action.target) return false;
                model_[screen.id].rgb = action.target;
                break;
            case Command::kTemp:
                model_[screen.id].kelvin = action.id;
                Persist(app);
                panel_->Adjust(model_[screen.id]);
                return true;
            case Command::kSwatch:
                model_[screen.id].hue = kSwatches[action.id].hue;
                model_[screen.id].saturation = kSwatches[action.id].saturation;
                Persist(app);
                panel_->Adjust(model_[screen.id]);
                return true;
        }
        Persist(app);
        Draw();
        return true;
    }

    Model model_;
    std::array<Screen, 4> pages_{};
    size_t depth_{};
    bool visible_{}, save_pending_{};
    std::unique_ptr<Panel> panel_;
    std::optional<mp::Timer> toast_, save_retry_;
};
}  // namespace
}  // namespace light_sequence

int main() {
    micropixel::Application app;
    app.renderer()
        .ConfigureDisplay({.logical_size = {light_sequence::kCanvas, light_sequence::kCanvas},
                           .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
        .value();
    light_sequence::LightActivity activity;
    app.RunComponents(&activity, nullptr);
    return 0;
}
