// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <optional>

#include "login_view.hpp"
#include "music_model.hpp"
#include "music_view.hpp"
namespace netease {
class MusicActivity final : public micropixel::Activity {
   public:
    explicit MusicActivity(MusicModel& model) : model_(model) {}
    void OnCreate(micropixel::ComponentContext& context) override {
        auto& m = model_;
        m.activity_token_ = context.token();
        if (!m.initialized_) {
            const auto renderer = m.app_.renderer();
            layout_ = MusicLayout::ForDisplay(renderer.physical_size());
            renderer
                .ConfigureDisplay({.logical_size = {uint32_t(layout_.width), uint32_t(layout_.height)},
                                   .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
                .value();
        }
        ui_ = std::make_unique<Ui>(m.app_, layout_);
        timer_.emplace(m.app_.timers().Every(micropixel::Duration::Milliseconds(100)).value());
        visible_ = false;
        if (!m.initialized_) m.Initialize([this] { Flush(true); });
        Flush(true);
        m.app_.log().Info("NETEASE_ACTIVITY created");
    }
    void OnResume(micropixel::ComponentContext&) override {
        visible_ = true;
        Flush(true);
        model_.app_.log().Info("NETEASE_ACTIVITY resumed");
    }
    void OnStop(micropixel::ComponentContext&) override {
        visible_ = false;
        if (ui_) {
            ui_->view.CancelInput();
            ui_->login.CancelInput();
        }
        model_.StopEditing();
        model_.app_.log().Info("NETEASE_ACTIVITY stopped");
    }
    void OnDestroy(micropixel::ComponentContext&) override {
        visible_ = false;
        model_.activity_token_ = 0;
        if (MusicModel::BrowseOperation(model_.client_.operation())) model_.read_pending_ = true;
        model_.client_.Cancel();
        model_.login_.Cancel();
        timer_.reset();
        ui_.reset();
        model_.app_.log().Info("NETEASE_ACTIVITY destroyed");
    }
    bool OnNavigateBack(micropixel::ComponentContext& context) override {
        auto& m = model_;
        if (m.login_visible_)
            m.CloseAccount();
        else if (m.state_.editing)
            m.StopEditing();
        else if (m.state_.screen == MusicScreen::kPlayer || m.state_.screen == MusicScreen::kCollection)
            m.Act({MusicActionKind::kBack});
        else {
            context.DestroyActivity().value();
            return true;
        }
        m.Draw();
        Flush(false);
        return true;
    }
    void OnEvent(micropixel::ComponentContext& context, const micropixel::Event& event) override {
        if (!ui_ || !visible_) return;
        auto& m = model_;
        bool changed = false;
        if (const auto* touch = event.touch()) {
            if (m.login_visible_) {
                const auto action = ui_->login.Touch(*touch, changed);
                if (action == LoginAction::kBack)
                    m.CloseAccount();
                else if (action == LoginAction::kRefresh) {
                    const bool remember = m.account_.remember();
                    m.ClearPrivate();
                    m.account_.Clear();
                    (void)m.account_.SetRemember(remember);
                    m.login_.Start();
                } else if (action == LoginAction::kRemember) {
                    if (!m.account_.SetRemember(!m.account_.remember())) m.login_.message = "无法保存登录状态";
                } else if (action == LoginAction::kLogout) {
                    m.ClearPrivate();
                    m.login_.Logout();
                    m.Message("已退出登录");
                }
                if (action != LoginAction::kNone) changed = true;
            } else {
                const auto action = ui_->view.Touch(*touch, m.client_, m.current_, changed);
                if (action.kind != MusicActionKind::kNone) {
                    m.Act(action);
                    changed = true;
                }
            }
        }
        if (const auto* key = event.key();
            key && (key->phase() == KeyPhase::kDown || key->phase() == KeyPhase::kRepeat)) {
            if (key->code() == KeyCode::kBack && m.login_visible_) {
                m.CloseAccount();
                changed = true;
            } else if (!m.login_visible_ && key->code() == KeyCode::kConfirm) {
                m.Act({m.state_.editing ? MusicActionKind::kSubmit : MusicActionKind::kToggle});
                changed = true;
            } else if (m.state_.editing && key->code() == KeyCode::kBack && !m.state_.query.empty()) {
                auto n = m.state_.query.size() - 1;
                while (n && (uint8_t(m.state_.query[n]) & 0xc0) == 0x80) --n;
                m.state_.query.resize(n);
                changed = true;
            }
        }
        if (timer_ && event.TimerFrom(*timer_)) {
            if (m.state_.editing) {
                auto text = m.app_.input().ReadText();
                if (text && !text->empty() && m.state_.query.size() + text->size() <= 128) {
                    m.state_.query += text->c_str();
                    changed = true;
                }
            }
        }
        if (changed) m.Draw();
        Flush(false);
        if (m.app_.launch_arguments().HasFlag("--destroy-activity") && !m.destroy_test_done_ && m.playback_.valid()) {
            m.destroy_test_done_ = true;
            context.DestroyActivity().value();
        }
    }

   private:
    struct Ui {
        micropixel::Scene scene;
        MusicView view;
        LoginView login;
        Ui(micropixel::Application& app, MusicLayout layout)
            : scene(app.renderer().CreateScene(micropixel::ui::panel::Background).value()),
              view(scene, layout),
              login(scene, layout) {}
    };
    void Flush(bool force) {
        auto& m = model_;
        if (!ui_ || (!force && seen_ == m.dirty_)) return;
        if (force || avatar_ != m.avatar_revision_) {
            ui_->view.ClearImages();
            avatar_ = m.avatar_revision_;
            cover_ = UINT32_MAX;
            if (!m.avatar_image_.empty() && ui_->view.SetAvatar(m.app_, m.avatar_image_))
                m.app_.log().Info("NETEASE_HOME avatar ready");
        }
        if (force || cover_ != m.cover_revision_) {
            ui_->view.ClearCover();
            cover_ = m.cover_revision_;
            if (!m.cover_image_.empty() && ui_->view.SetCover(m.app_, m.cover_image_))
                m.app_.log().Info("NETEASE_PLAYER cover ready");
        }
        ui_->view.Update(m.account_, m.client_, m.state_, m.current_, m.media_, m.playback_.valid());
        ui_->login.Show(m.login_visible_);
        if (m.login_visible_) ui_->login.Update(m.login_, m.account_);
        m.app_.renderer().Present(ui_->scene).value();
        seen_ = m.dirty_;
    }
    MusicModel& model_;
    MusicLayout layout_;
    std::unique_ptr<Ui> ui_;
    std::optional<micropixel::Timer> timer_;
    uint32_t seen_{}, avatar_{}, cover_{};
    bool visible_{};
};
class PlaybackService final : public micropixel::AppService {
   public:
    explicit PlaybackService(MusicModel& model) : model_(model) {}
    void OnStart(micropixel::ComponentContext& context) override {
        model_.service_token_ = context.token();
        timer_.emplace(model_.app_.timers().Every(micropixel::Duration::Milliseconds(100)).value());
        model_.app_.log().Info("NETEASE_SERVICE started");
        model_.PumpPlayback();
        model_.PublishWidget(true);
    }
    void OnStop(micropixel::ComponentContext&) override {
        model_.FlushPlayback();
        model_.play_client_.Cancel();
        model_.playback_.Close();
        timer_.reset();
        model_.service_token_ = 0;
        model_.app_.log().Info("NETEASE_SERVICE stopped");
    }
    void OnEvent(micropixel::ComponentContext&, const micropixel::Event& event) override {
        if (const auto* focus = event.audio_focus())
            model_.app_.log().Info(focus->interrupted ? "NETEASE_FOCUS ducked" : "NETEASE_FOCUS restored");
        if (const auto* incoming = event.message()) {
            const auto message = model_.app_.messages().Read(incoming->ticket);
            if (message) (void)model_.app_.messages().Ack(incoming->ticket, model_.Command(*message));
            return;
        }
        if (timer_ && event.TimerFrom(*timer_)) model_.Tick();
    }

   private:
    MusicModel& model_;
    std::optional<micropixel::Timer> timer_;
};
}  // namespace netease
