// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "article_client.hpp"
#include "sdk/ui/material_icons.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/scroll_view.hpp"

namespace news {
class ArticleReader final {
   public:
    enum class Action { None, Back, NewsChannel };
    ArticleReader(Container& parent, const ui::MaterialAtlas& icons) {
        using namespace ui::panel;
        root_ = parent.CreateContainer({.z_order = 100, .visible = false}).value();
        (void)root_.CreateShape({0, 0, 720, 720}, Background).value();
        heading_ = root_.CreateLabel({96, 21}, "新闻正文", Text, SystemFont::kLarge).value();
        back_ = ui::MaterialIconButton::Create(root_, icons, {0, 0, 84, 84}, ui::MaterialSymbol::Back, Background);
        refresh_ =
            ui::MaterialIconButton::Create(root_, icons, {612, 0, 84, 84}, ui::MaterialSymbol::Refresh, Background);
        card_ = root_.CreateRoundedRect({24, 96, 672, 510}, {.fill = Surface, .radius = 24}).value();
        scroll_ = root_.CreateScrollView({48, 120, 624, 462});
        for (auto& label : labels_)
            label = scroll_.content().CreateLabel({0, 0}, " ", Text, SystemFont::kLarge).value();
        footer_ = root_.CreateLabel({24, 610}, " ", Muted, SystemFont::kSmall).value();
        done_ = root_.CreateTextButton({.bounds = {24, 636, 672, 63}, .text = "返回列表", .style = Style()});
        done_.SetHitPadding(11);
    }
    ~ArticleReader() {
        if (root_.valid()) root_.Destroy().value();
    }
    bool visible() const { return visible_; }
    void Open(const Application& app, const Article& article, Source source) {
        Stop();
        article_ = article;
        source_ = source;
        visible_ = true;
        offline_ = false;
        doc_ = {};
        lines_.clear();
        next_ = 0;
        layout_ = false;
        heading_.SetText(source == Source::Cctv ? "新闻正文" : "头条话题");
        done_.SetText(source == Source::Cctv ? "返回列表" : "阅读新闻正文").value();
        refresh_.SetVisible(source == Source::Cctv);
        root_.SetVisible(true);
        scroll_.ScrollTo(0);
        Base(app);
        if (source == Source::Toutiao) {
            AddWrapped(app, "头条热榜提供话题标题与热度。请切换到央视新闻频道阅读正文。", ui::panel::Muted);
            Footer("话题速览 · 未提供正文");
            Sync(app, true);
            return;
        }
        LoadCache(app);
        if (!doc_.text.empty())
            BeginLayout(app);
        else {
            AddLine("正在下载新闻正文…", ui::panel::Muted);
            Sync(app, true);
        }
        Fetch(app);
    }
    void Stop() {
        client_.Cancel();
        if (busy_) retry_ = true;
        busy_ = false;
        scroll_.Cancel();
        back_.Reset();
        refresh_.Reset();
        done_.Reset();
    }
    void Resume(const Application& app) {
        if (retry_ && source_ == Source::Cctv)
            Fetch(app);
        else
            Sync(app, true);
    }
    void Close() {
        Stop();
        visible_ = false;
        layout_ = false;
        root_.SetVisible(false);
    }
    Action Touch(const Application& app, const TouchEvent& touch) {
        auto done = done_.OnTouch(touch);
        if (back_.OnTouch(touch).clicked || done.clicked) {
            bool change = done.clicked && source_ == Source::Toutiao;
            Close();
            return change ? Action::NewsChannel : Action::Back;
        }
        if (refresh_.OnTouch(touch).clicked && !busy_) {
            Fetch(app);
            return Action::None;
        }
        auto update = scroll_.OnTouch(touch);
        if (update.changed) {
            Bind(false);
            app.renderer().Present(*scene_).value();
        } else if (done.visual_changed)
            app.renderer().Present(*scene_).value();
        return Action::None;
    }
    void SetScene(Scene& scene) { scene_ = &scene; }
    void Tick(const Application& app) {
        if (!visible_) return;
        if (busy_) {
            Document candidate;
            int result = client_.Poll(candidate);
            if (!result && app.clock().Now().microseconds() - started_ < 30000000) {
                if (layout_) LayoutStep(app);
                return;
            }
            busy_ = false;
            client_.Cancel();
            refresh_.SetEnabled(true);
            if (result == 1) {
                doc_ = std::move(candidate);
                offline_ = false;
                retry_ = false;
                SaveCache(app);
                BeginLayout(app);
                FixedString<64> log;
                log.Append("NEWS article success: bytes=");
                log.AppendUint(doc_.text.size());
                app.log().Info(log.c_str());
            } else {
                retry_ = true;
                if (doc_.text.empty()) {
                    Base(app);
                    AddWrapped(app, "正文暂时无法下载，请点击右上角重试。", ui::panel::Muted);
                    Footer("正文下载失败 · 可重试");
                    Sync(app, true);
                } else {
                    offline_ = true;
                    Footer(doc_.truncated ? "离线正文缓存 · 节选" : "离线正文缓存");
                    Sync(app, true);
                }
                app.log().Info("NEWS article failed; cache retained");
            }
        }
        if (layout_) LayoutStep(app);
    }

   private:
    struct Line {
        std::string text;
        int y{};
        Color color{Color::White()};
    };
    struct SavedBody {
        uint32_t version{1}, size{}, truncated{};
        char path[64]{}, source[64]{}, text[3073]{};
    };
    static_assert(sizeof(SavedBody) <= 4096);
    void Footer(const char* state) {
        FixedString<160> text;
        text.Append(doc_.source.empty() ? SourceName(source_) : doc_.source.c_str());
        text.Append(" · ");
        text.Append(state);
        footer_.SetText(text.c_str());
    }
    void AddLine(std::string text, Color color) {
        if (lines_.size() >= 768) {
            doc_.truncated = true;
            next_ = doc_.text.size();
            return;
        }
        lines_.push_back({std::move(text), height_, color});
        height_ += 38;
    }
    void AddWrapped(const Application& app, std::string_view text, Color color) {
        auto lines = ui::WrapText<8>(text, 624, [&](std::string_view value) {
            auto m = app.renderer().MeasureText(std::string(value).c_str(), SystemFont::kLarge);
            return m ? m->width : UINT32_MAX;
        });
        for (auto& line : lines)
            if (!line.empty()) AddLine(std::move(line), color);
    }
    void Base(const Application& app) {
        lines_.clear();
        height_ = 0;
        first_ = UINT32_MAX;
        AddWrapped(app, article_.title, ui::panel::Text);
        height_ += 22;
    }
    void BeginLayout(const Application& app) {
        Base(app);
        next_ = 0;
        layout_ = !doc_.text.empty();
        if (doc_.media_only) {
            AddWrapped(app, "此条新闻以视频为主，暂时没有文字正文。", ui::panel::Muted);
            Footer("视频新闻 · 暂无文字正文");
        } else
            Footer(offline_ ? "正在排版缓存…" : "正在排版正文…");
        Sync(app, true);
    }
    void LayoutStep(const Application& app) {
        unsigned budget = 6;
        while (budget-- && next_ < doc_.text.size() && lines_.size() < 768) {
            if (doc_.text[next_] == '\n') {
                height_ += 14;
                ++next_;
                continue;
            }
            while (next_ < doc_.text.size() && doc_.text[next_] == ' ') ++next_;
            if (next_ == doc_.text.size()) break;
            auto remaining = std::string_view(doc_.text).substr(next_);
            auto count = ui::FittingTextPrefix(remaining, 624, [&](std::string_view text) {
                auto m = app.renderer().MeasureText(std::string(text).c_str(), SystemFont::kLarge);
                return m ? m->width : UINT32_MAX;
            });
            if (!count) {
                if (remaining.starts_with('\n')) {
                    ++next_;
                    continue;
                }
                doc_.truncated = true;
                next_ = doc_.text.size();
                break;
            }
            AddLine(std::string(remaining.substr(0, count)), ui::panel::Text);
            next_ += count;
        }
        if (next_ >= doc_.text.size() || lines_.size() >= 768) {
            layout_ = false;
            if (next_ < doc_.text.size()) doc_.truncated = true;
            Footer(offline_ ? (doc_.truncated ? "离线正文缓存 · 节选" : "离线正文缓存")
                            : (doc_.truncated ? "正文已到显示上限" : "正文 · 可上下滚动"));
            app.log().Info("NEWS article layout ready");
        }
        Sync(app, true);
    }
    void Fetch(const Application& app) {
        retry_ = false;
        busy_ = true;
        started_ = app.clock().Now().microseconds();
        refresh_.SetEnabled(false);
        if (!client_.Start(app, article_.path)) {
            busy_ = false;
            retry_ = true;
            refresh_.SetEnabled(true);
            Footer("正文链接不可用 · 请刷新列表");
        } else {
            Footer("正在下载正文…");
            app.log().Info("NEWS article started");
        }
        Sync(app, true);
    }
    void Sync(const Application& app, bool force) {
        scroll_.SetContentHeight(height_);
        card_.SetRect({24, 96, 672, std::clamp(height_ + 48, 220, 510)});
        Bind(force);
        app.renderer().Present(*scene_).value();
    }
    void Bind(bool force) {
        unsigned first = 0;
        while (first + 1 < lines_.size() && lines_[first + 1].y <= scroll_.offset()) ++first;
        if (first == first_ && !force) return;
        first_ = first;
        for (unsigned i = 0; i < labels_.size(); ++i) {
            bool shown = first + i < lines_.size();
            labels_[i].SetVisible(shown);
            if (shown) {
                auto& line = lines_[first + i];
                labels_[i].SetPosition({0, line.y});
                labels_[i].SetText(line.text.empty() ? " " : line.text.c_str());
                labels_[i].SetColor(line.color);
            }
        }
    }
    void LoadCache(const Application& app) {
        auto saved = std::make_unique<SavedBody>();
        auto n = app.storage().GetBytes("news.body", reinterpret_cast<uint8_t*>(saved.get()), sizeof(SavedBody));
        if (!n || *n != sizeof(SavedBody) || saved->version != 1 || saved->size > 3072 || saved->path[63] ||
            saved->source[63] || saved->text[saved->size] || saved->path != std::string_view(article_.path))
            return;
        auto text = std::string_view(saved->text, saved->size);
        if (!ui::ValidUtf8(text) || text.find('\0') != text.npos || !ValidTitle(saved->source)) return;
        doc_.text = text;
        doc_.source = saved->source;
        doc_.truncated = saved->truncated;
        offline_ = true;
        app.log().Info("NEWS article cache loaded");
    }
    void SaveCache(const Application& app) {
        if (doc_.text.empty()) return;
        auto saved = std::make_unique<SavedBody>();
        saved->size = Utf8Prefix(doc_.text, 3072);
        saved->truncated = doc_.truncated || saved->size < doc_.text.size();
        std::memcpy(saved->path, article_.path, sizeof(saved->path));
        auto source = doc_.source.empty() ? std::string("央视网") : doc_.source;
        std::memcpy(saved->source, source.data(), std::min<size_t>(63, source.size()));
        std::memcpy(saved->text, doc_.text.data(), saved->size);
        (void)app.storage().SetBytes("news.body", reinterpret_cast<const uint8_t*>(saved.get()), sizeof(SavedBody));
    }
    ContainerNode root_;
    ui::ScrollView scroll_;
    ui::MaterialIconButton back_, refresh_;
    ui::TextButton done_;
    LabelNode heading_, footer_;
    RoundedRectNode card_;
    std::array<LabelNode, 16> labels_;
    std::vector<Line> lines_;
    ArticleClient client_;
    Article article_;
    Source source_{Source::Cctv};
    Document doc_;
    Scene* scene_{};
    unsigned first_{UINT32_MAX};
    size_t next_{};
    int height_{};
    uint64_t started_{};
    bool visible_{}, busy_{}, layout_{}, retry_{}, offline_{};
};
}  // namespace news
