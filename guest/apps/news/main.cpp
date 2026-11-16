// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "article_reader.hpp"
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
        badge_label_ = root_.CreateLabel({600, 95}, "新闻", Muted, SystemFont::kSmall, true).value();
        for (unsigned i = 0; i < 2; ++i)
            skeleton_[i] =
                root_.CreateRoundedRect({24, 28 + int(i) * 40, i ? 324 : 456, 22}, {.fill = Line, .radius = 8}).value();
        hit_ = ui::Button({0, 0, 672, 161});
    }
    void Bind(const Application& app, const Article* article, unsigned rank, int y, bool read, Source source) {
        root_.SetVisible(true);
        root_.SetTranslation({0, y});
        enabled_ = article;
        Reset();
        for (auto& s : skeleton_) s.SetVisible(!article);
        if (!article) {
            for (auto& t : title_) t.SetText(" ");
            rank_.SetText(" ");
            meta_.SetText("正在获取新闻列表…");
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
        if (source == Source::Toutiao) {
            meta.Append("热度 ");
            meta.Append(HeatText(article->heat).c_str());
            meta.Append(read ? " · 已读" : " · 今日头条");
        } else {
            meta.Append("央视网 · ");
            meta.Append(read ? "已读" : "阅读全文");
        }
        badge_label_.SetText(source == Source::Cctv ? "正文" : "热榜");
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
    LabelNode meta_, rank_, badge_label_;
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
        heading_ = scene_->CreateLabel({24, 21}, "新闻 · 央视", Text, SystemFont::kLarge).value();
        source_hit_ = ui::Button({24, 0, 300, 64});
        (void)ui::MaterialIcon::Create(*scene_, *icons_, {280, 25}, 26, ui::MaterialSymbol::Right);
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
        reader_ = std::make_unique<ArticleReader>(*scene_, *icons_);
        reader_->SetScene(*scene_);
        source_menu_ = scene_->CreateContainer({.z_order = 200, .visible = false}).value();
        (void)source_menu_.CreateShape({0, 0, 720, 720}, Background).value();
        (void)source_menu_.CreateLabel({96, 21}, "新闻来源", Text, SystemFont::kLarge).value();
        source_back_ =
            ui::MaterialIconButton::Create(source_menu_, *icons_, {0, 0, 84, 84}, ui::MaterialSymbol::Back, Background);
        sources_[0] = source_menu_.CreateTextButton(
            {.bounds = {24, 120, 672, 96}, .text = "央视新闻 · 阅读正文", .style = Style()});
        sources_[1] = source_menu_.CreateTextButton(
            {.bounds = {24, 240, 672, 96}, .text = "头条热榜 · 话题速览", .style = Style()});
        Rebuild(app, true);
        Draw(app);
        app.log().Info("NEWS activity created");
    }
    void OnResume(ComponentContext& c) override {
        auto& app = c.app();
        timer_.emplace(app.timers().Every(Duration::Milliseconds(100)).value());
        app.log().Info("NEWS activity resumed");
        if (reader_->visible()) {
            reader_->Resume(app);
            return;
        }
        if (!last_success_ || model_.failed || app.clock().Now().microseconds() - last_success_ >= 300000000)
            Start(app);
        else
            Draw(app);
    }
    void OnStop(ComponentContext& c) override {
        client_.Cancel();
        reader_->Stop();
        if (model_.busy) last_success_ = 0;
        model_.busy = false;
        timer_.reset();
        Reset();
        c.app().log().Info("NEWS activity stopped");
    }
    void OnDestroy(ComponentContext&) override {
        client_.Cancel();
        timer_.reset();
        reader_.reset();
        scene_.reset();
        icons_.reset();
    }
    void Reset() {
        scroll_.Cancel();
        refresh_.Reset();
        source_hit_.Reset();
        source_back_.Reset();
        for (auto& source : sources_) source.Reset();
        for (auto& row : rows_) row.Reset();
        for (auto& tab : tab_hits_) tab.Reset();
    }
    void Start(const Application& app) {
        if (model_.busy) return;
        model_.busy = true;
        model_.failed = false;
        started_ = app.clock().Now().microseconds();
        if (!client_.Start(app, model_.source)) {
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
        if (timer_ && event.TimerFrom(*timer_) && reader_->visible()) {
            reader_->Tick(app);
            return;
        }
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
            if (reader_->visible()) {
                auto action = reader_->Touch(app, *touch);
                if (action == ArticleReader::Action::NewsChannel)
                    SwitchSource(app, Source::Cctv);
                else if (action == ArticleReader::Action::Back) {
                    Rebuild(app, false);
                    Draw(app);
                    app.log().Info("NEWS detail closed");
                }
                return;
            }
            if (source_open_) {
                if (source_back_.OnTouch(*touch).clicked) {
                    source_open_ = false;
                    source_menu_.SetVisible(false);
                    Draw(app);
                    return;
                }
                bool changed = false;
                for (unsigned i = 0; i < 2; ++i) {
                    auto hit = sources_[i].OnTouch(*touch);
                    changed |= hit.visual_changed;
                    if (hit.clicked) {
                        SwitchSource(app, i == 0 ? Source::Cctv : Source::Toutiao);
                        return;
                    }
                }
                if (changed) app.renderer().Present(*scene_).value();
                return;
            }
            if (source_hit_.OnTouch(*touch).clicked) {
                source_open_ = true;
                sources_[0].SetStyle(Style(model_.source == Source::Cctv)).value();
                sources_[1].SetStyle(Style(model_.source == Source::Toutiao)).value();
                source_menu_.SetVisible(true);
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
                rows_[i].Bind(app, nullptr, 0, int(item) * 176, false, model_.source);
            else if (item < visible_count_) {
                unsigned index = indices_[item];
                const auto& article = model_.articles[index];
                rows_[i].Bind(app, &article, index + 1, int(item) * 176, model_.Read(article.key), model_.source);
            } else
                rows_[i].Hide();
        }
    }
    void Draw(const Application& app) {
        heading_.SetText(model_.source == Source::Cctv ? "新闻 · 央视" : "资讯 · 头条");
        tab_text_[0].SetText(model_.source == Source::Cctv ? "新闻" : "热榜");
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
        status.Append(model_.failed || model_.cached  ? "上次内容 · "
                      : model_.source == Source::Cctv ? "央视网 · "
                                                      : "今日头条 · ");
        status.AppendUint(model_.articles.size());
        status.Append(model_.source == Source::Cctv ? " 条 · 点击阅读全文" : " 条 · 点击查看话题");
        status_.SetText(status.c_str());
        app.renderer().Present(*scene_).value();
    }
    void OpenDetail(const Application& app, unsigned index) {
        client_.Cancel();
        model_.busy = false;
        const auto& article = model_.articles[index];
        model_.MarkRead(app, article.key);
        reader_->Open(app, article, model_.source);
        FixedString<64> log;
        log.Append("NEWS detail opened: ");
        log.AppendUint(index + 1);
        app.log().Info(log.c_str());
    }
    void SwitchSource(const Application& app, Source source) {
        client_.Cancel();
        reader_->Close();
        model_.Choose(app, source);
        source_open_ = false;
        source_menu_.SetVisible(false);
        filter_ = 0;
        last_success_ = 0;
        Reset();
        Rebuild(app, true);
        Start(app);
        app.log().Info(source == Source::Cctv ? "NEWS source cctv" : "NEWS source toutiao");
    }
    Model& model_;
    NewsClient client_;
    std::optional<Scene> scene_;
    std::unique_ptr<ui::MaterialAtlas> icons_;
    std::optional<Timer> timer_;
    LabelNode heading_, updated_, empty_, status_;
    std::array<LabelNode, 3> tab_text_;
    std::array<RoundedRectNode, 3> tab_bg_;
    std::array<ui::Button, 3> tab_hits_;
    ui::MaterialIconButton refresh_, source_back_;
    ui::Button source_hit_;
    ContainerNode source_menu_;
    std::array<ui::TextButton, 2> sources_;
    std::unique_ptr<ArticleReader> reader_;
    ui::ScrollView scroll_;
    std::array<NewsCard, 4> rows_;
    std::array<unsigned, MaxArticles> indices_{};
    unsigned visible_count_{}, filter_{}, first_{UINT32_MAX};
    uint64_t last_success_{}, started_{};
    bool skeleton_{}, source_open_{};
};
}  // namespace news
int main() {
    micropixel::Application app;
    auto model = std::make_unique<news::Model>();
    auto activity = std::make_unique<news::NewsActivity>(*model);
    app.RunComponents(activity.get(), nullptr);
    return 0;
}
