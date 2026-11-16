// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "news_client.hpp"
#include "sdk/ui/material_icons.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/scroll_view.hpp"

namespace news {
using namespace ui::panel;
class NewsCard {
   public:
    void Create(Container& parent) {
        root_ = parent.CreateContainer({.clip = {0, 0, 672, 161}}).value();
        Card(root_, {0, 0, 672, 161});
        feedback_ = root_.CreateRoundedRect({0, 0, 672, 161}, {.fill = Accent, .radius = 24, .opacity = 0}).value();
        for (unsigned i = 0; i < 2; ++i)
            title_[i] = root_.CreateLabel({24, 21 + int(i) * 38}, " ", Text, SystemFont::kLarge).value();
        meta_ = root_.CreateLabel({24, 113}, " ", Muted, SystemFont::kSmall).value();
        badge_ = root_.CreateRoundedRect({552, 32, 96, 96}, {.fill = Raised, .radius = 18}).value();
        rank_ = root_.CreateLabel({600, 47}, " ", Accent, SystemFont::kTitle, true).value();
        (void)root_.CreateLabel({600, 95}, "热榜", Muted, SystemFont::kSmall, true).value();
        for (unsigned i = 0; i < 2; ++i)
            skeleton_[i] =
                root_.CreateRoundedRect({24, 28 + int(i) * 40, i ? 324 : 456, 22}, {.fill = Line, .radius = 8}).value();
        hit_ = ui::Button({0, 0, 672, 161});
    }
    void Bind(const Application& app, const Article* article, unsigned rank, int y, bool read) {
        root_.SetVisible(true);
        root_.SetTranslation({0, y});
        enabled_ = article;
        Reset();
        for (auto& s : skeleton_) s.SetVisible(!article);
        if (!article) {
            for (auto& t : title_) t.SetText(" ");
            rank_.SetText(" ");
            meta_.SetText("正在获取今日头条热榜…");
            return;
        }
        auto lines = ui::WrapText<2>(article->title, 504, [&](std::string_view text) {
            auto measured = app.renderer().MeasureText(std::string(text).c_str(), SystemFont::kLarge);
            return measured ? measured->width : UINT32_MAX;
        });
        for (unsigned i = 0; i < 2; ++i) {
            title_[i].SetText(lines[i].empty() ? " " : lines[i].c_str());
            title_[i].SetColor(read ? Muted : Text);
        }
        FixedString<96> meta;
        meta.Append("热度 ");
        meta.Append(HeatText(article->heat).c_str());
        meta.Append(read ? " · 已读" : " · 今日头条");
        meta_.SetText(meta.c_str());
        FixedString<16> number;
        number.AppendUint(rank);
        rank_.SetText(number.c_str());
        badge_.SetFillColor(rank <= 3 ? Color::Rgb(52, 42, 31) : Raised);
        rank_.SetColor(rank <= 3 ? Accent : Text);
    }
    void Hide() {
        root_.SetVisible(false);
        Reset();
        enabled_ = false;
    }
    void Reset() {
        hit_.Reset();
        feedback_.SetOpacity(0);
    }
    ui::ButtonUpdate Touch(const TouchEvent& touch) {
        if (!enabled_) return {};
        auto update = hit_.OnTouch(touch.WithPosition(root_.ToLocal(touch.position())));
        if (update.visual_changed) feedback_.SetOpacity(hit_.pressed() ? 20 : 0);
        return update;
    }

   private:
    ContainerNode root_;
    ui::Button hit_;
    std::array<LabelNode, 2> title_;
    LabelNode meta_, rank_;
    RoundedRectNode badge_, feedback_;
    std::array<RoundedRectNode, 2> skeleton_;
    bool enabled_{};
};
class NewsActivity final : public Activity {
   public:
    explicit NewsActivity(Model& model) : model_(model) {}

   private:
    void OnCreate(ComponentContext& c) override {
        const auto& app = c.app();
        model_.Load(app);
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        scene_.emplace(app.renderer().CreateScene(Background).value());
        icons_ = std::make_unique<ui::MaterialAtlas>(app);
        (void)scene_->CreateLabel({24, 21}, "资讯", Text, SystemFont::kLarge).value();
        updated_ = scene_->CreateLabel({350, 23}, "尚未更新", Muted, SystemFont::kMedium).value();
        refresh_ =
            ui::MaterialIconButton::Create(*scene_, *icons_, {612, 0, 84, 84}, ui::MaterialSymbol::Refresh, Background);
        constexpr const char* titles[]{"热榜", "未读", "已读"};
        for (unsigned i = 0; i < 3; ++i) {
            int x = 24 + int(i) * 230;
            tab_hits_[i] = ui::Button({x, 64, 212, 84});
            tab_bg_[i] = scene_->CreateRoundedRect({x, 78, 212, 56}, {.fill = Surface, .radius = 28}).value();
            tab_text_[i] = scene_->CreateLabel({x + 106, 93}, titles[i], Text, SystemFont::kMedium, true).value();
        }
        scroll_ = scene_->CreateScrollView({24, 155, 672, 528});
        for (auto& row : rows_) row.Create(scroll_.content());
        empty_ = scene_->CreateLabel({360, 330}, "暂无热榜", Muted, SystemFont::kLarge, true).value();
        status_ = scene_->CreateLabel({24, 696}, "来源：今日头条 · 标题与热度", Muted, SystemFont::kSmall).value();
        detail_ = scene_->CreateContainer({.z_order = 100, .visible = false}).value();
        (void)detail_.CreateShape({0, 0, 720, 720}, Background).value();
        detail_back_ =
            ui::MaterialIconButton::Create(detail_, *icons_, {0, 0, 84, 84}, ui::MaterialSymbol::Back, Background);
        (void)detail_.CreateLabel({96, 21}, "热榜速览", Text, SystemFont::kLarge).value();
        Card(detail_, {24, 96, 672, 480});
        for (unsigned i = 0; i < detail_lines_.size(); ++i)
            detail_lines_[i] = detail_.CreateLabel({48, 128 + int(i) * 38}, " ", Text, SystemFont::kLarge).value();
        detail_meta_ = detail_.CreateLabel({48, 490}, " ", Accent, SystemFont::kMedium).value();
        (void)detail_.CreateLabel({48, 530}, "来源：今日头条热榜", Muted, SystemFont::kMedium).value();
        (void)detail_.CreateLabel({24, 600}, "这里显示热榜标题与热度", Muted, SystemFont::kMedium).value();
        detail_done_ = detail_.CreateTextButton({.bounds = {24, 636, 672, 63}, .text = "返回热榜", .style = Style()});
        detail_done_.SetHitPadding(11);
        Rebuild(app, true);
        Draw(app);
        app.log().Info("NEWS activity created");
    }
    void OnResume(ComponentContext& c) override {
        auto& app = c.app();
        timer_.emplace(app.timers().Every(Duration::Milliseconds(100)).value());
        app.log().Info("NEWS activity resumed");
        if (!last_success_ || model_.failed || app.clock().Now().microseconds() - last_success_ >= 300000000)
            Start(app);
        else
            Draw(app);
    }
    void OnStop(ComponentContext& c) override {
        client_.Cancel();
        if (model_.busy) last_success_ = 0;
        model_.busy = false;
        timer_.reset();
        Reset();
        c.app().log().Info("NEWS activity stopped");
    }
    void OnDestroy(ComponentContext&) override {
        client_.Cancel();
        timer_.reset();
        scene_.reset();
        icons_.reset();
    }
    void Reset() {
        scroll_.Cancel();
        refresh_.Reset();
        detail_back_.Reset();
        detail_done_.Reset();
        for (auto& row : rows_) row.Reset();
        for (auto& tab : tab_hits_) tab.Reset();
    }
    void Start(const Application& app) {
        if (model_.busy) return;
        model_.busy = true;
        model_.failed = false;
        started_ = app.clock().Now().microseconds();
        if (!client_.Start(app)) {
            model_.busy = false;
            model_.failed = true;
            app.log().Info("NEWS refresh failed");
        } else
            app.log().Info("NEWS refresh started");
        Rebuild(app, false);
        Draw(app);
    }
    void OnEvent(ComponentContext& c, const Event& event) override {
        const auto& app = c.app();
        if (timer_ && event.TimerFrom(*timer_) && model_.busy) {
            int result = client_.Poll(model_.articles);
            if (!result && app.clock().Now().microseconds() - started_ < 30000000) return;
            model_.busy = false;
            model_.failed = result != 1;
            client_.Cancel();
            if (result == 1) {
                last_success_ = app.clock().Now().microseconds();
                auto wall = app.jobs().Now();
                model_.updated = wall && wall->valid ? wall->utc : 0;
                model_.cached = false;
                model_.Save(app);
                FixedString<64> log;
                log.Append("NEWS refresh success: ");
                log.AppendUint(model_.articles.size());
                app.log().Info(log.c_str());
            } else
                app.log().Info("NEWS refresh failed; cache retained");
            Rebuild(app, result == 1);
            Draw(app);
        }
        if (auto* touch = event.touch()) {
            if (detail_visible_) {
                auto done = detail_done_.OnTouch(*touch);
                if (detail_back_.OnTouch(*touch).clicked || done.clicked) {
                    detail_visible_ = false;
                    detail_.SetVisible(false);
                    Rebuild(app, false);
                    Draw(app);
                    app.log().Info("NEWS detail closed");
                } else if (done.visual_changed)
                    app.renderer().Present(*scene_).value();
                return;
            }
            if (refresh_.OnTouch(*touch).clicked) {
                Start(app);
                return;
            }
            for (unsigned i = 0; i < 3; ++i)
                if (tab_hits_[i].OnTouch(*touch).clicked) {
                    filter_ = i;
                    Reset();
                    Rebuild(app, true);
                    Draw(app);
                    app.log().Info(i == 0 ? "NEWS filter all" : i == 1 ? "NEWS filter unread" : "NEWS filter read");
                    return;
                }
            auto scroll = scroll_.OnTouch(*touch);
            if (scroll.dragging) {
                for (auto& row : rows_) row.Reset();
                Bind(app);
                app.renderer().Present(*scene_).value();
                return;
            }
            if (!scroll_.contains(touch->position())) return;
            bool redraw = false;
            for (unsigned i = 0; i < rows_.size(); ++i) {
                auto result = rows_[i].Touch(*touch);
                redraw |= result.visual_changed;
                if (result.clicked && first_ + i < visible_count_) {
                    OpenDetail(app, indices_[first_ + i]);
                    return;
                }
            }
            if (redraw) app.renderer().Present(*scene_).value();
        }
    }
    void Rebuild(const Application& app, bool top) {
        visible_count_ = 0;
        for (unsigned i = 0; i < model_.articles.size(); ++i) {
            bool read = model_.Read(model_.articles[i].key);
            if (!filter_ || (filter_ == 1 && !read) || (filter_ == 2 && read)) indices_[visible_count_++] = i;
        }
        skeleton_ = model_.articles.empty() && model_.busy;
        scroll_.SetContentHeight(int(skeleton_ ? 3 : visible_count_) * 176 - (skeleton_ || visible_count_ ? 15 : 0));
        if (top) scroll_.ScrollTo(0);
        first_ = UINT32_MAX;
        Bind(app);
    }
    void Bind(const Application& app) {
        unsigned first = unsigned(scroll_.offset()) / 176;
        if (first_ == first) return;
        first_ = first;
        for (unsigned i = 0; i < rows_.size(); ++i) {
            unsigned item = first + i;
            if (skeleton_ && item < 3)
                rows_[i].Bind(app, nullptr, 0, int(item) * 176, false);
            else if (item < visible_count_) {
                unsigned index = indices_[item];
                const auto& article = model_.articles[index];
                rows_[i].Bind(app, &article, index + 1, int(item) * 176, model_.Read(article.key));
            } else
                rows_[i].Hide();
        }
    }
    void Draw(const Application& app) {
        FixedString<64> updated;
        if (model_.busy)
            updated.Append("正在更新…");
        else if (model_.failed)
            updated.Append("更新失败 · 可重试");
        else if (model_.updated) {
            auto minutes = ((model_.updated + 28800) % 86400) / 60;
            if (minutes / 60 < 10) updated.Append("0");
            updated.AppendUint(minutes / 60);
            updated.Append(":");
            if (minutes % 60 < 10) updated.Append("0");
            updated.AppendUint(minutes % 60);
            updated.Append(model_.cached ? " 缓存" : " 更新");
        } else
            updated.Append(model_.cached ? "上次浏览" : "尚未更新");
        updated_.SetText(updated.c_str());
        refresh_.SetEnabled(!model_.busy);
        for (unsigned i = 0; i < 3; ++i) {
            tab_bg_[i].SetFillColor(i == filter_ ? Accent : Surface);
            tab_text_[i].SetColor(i == filter_ ? OnAccent : Muted);
        }
        empty_.SetVisible(!visible_count_ && !skeleton_);
        empty_.SetText(model_.failed  ? "暂时无法更新，点击右上角重试"
                       : filter_ == 2 ? "还没有已读内容"
                       : filter_ == 1 ? "已读完本次热榜"
                                      : "暂无热榜内容");
        FixedString<96> status;
        status.Append(model_.failed || model_.cached ? "上次内容 · " : "今日头条 · ");
        status.AppendUint(model_.articles.size());
        status.Append(" 条 · 点击查看完整标题与热度");
        status_.SetText(status.c_str());
        app.renderer().Present(*scene_).value();
    }
    void OpenDetail(const Application& app, unsigned index) {
        const auto& article = model_.articles[index];
        model_.MarkRead(app, article.key);
        auto lines = ui::WrapText<8>(article.title, 624, [&](std::string_view text) {
            auto value = app.renderer().MeasureText(std::string(text).c_str(), SystemFont::kLarge);
            return value ? value->width : UINT32_MAX;
        });
        for (unsigned i = 0; i < lines.size(); ++i) detail_lines_[i].SetText(lines[i].empty() ? " " : lines[i].c_str());
        FixedString<96> meta;
        meta.Append("第 ");
        meta.AppendUint(index + 1);
        meta.Append(" 名 · 热度 ");
        meta.Append(HeatText(article.heat).c_str());
        detail_meta_.SetText(meta.c_str());
        detail_visible_ = true;
        detail_.SetVisible(true);
        Reset();
        app.renderer().Present(*scene_).value();
        FixedString<64> log;
        log.Append("NEWS detail opened: ");
        log.AppendUint(index + 1);
        app.log().Info(log.c_str());
    }
    Model& model_;
    NewsClient client_;
    std::optional<Scene> scene_;
    std::unique_ptr<ui::MaterialAtlas> icons_;
    std::optional<Timer> timer_;
    LabelNode updated_, empty_, status_, detail_meta_;
    std::array<LabelNode, 8> detail_lines_;
    std::array<LabelNode, 3> tab_text_;
    std::array<RoundedRectNode, 3> tab_bg_;
    std::array<ui::Button, 3> tab_hits_;
    ui::MaterialIconButton refresh_, detail_back_;
    ui::TextButton detail_done_;
    ContainerNode detail_;
    ui::ScrollView scroll_;
    std::array<NewsCard, 4> rows_;
    std::array<unsigned, MaxArticles> indices_{};
    unsigned visible_count_{}, filter_{}, first_{UINT32_MAX};
    uint64_t last_success_{}, started_{};
    bool skeleton_{}, detail_visible_{};
};
}  // namespace news
int main() {
    micropixel::Application app;
    auto model = std::make_unique<news::Model>();
    auto activity = std::make_unique<news::NewsActivity>(*model);
    app.RunComponents(activity.get(), nullptr);
    return 0;
}
