// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>

#include "client.hpp"
#include "music_state.hpp"
#include "sdk/micropixel.hpp"
#include "view_text.hpp"

namespace netease {
inline std::string TimeText(uint32_t ms) {
    auto s = ms / 1000;
    return Decimal(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + Decimal(s % 60);
}
class MusicView final {
   public:
    explicit MusicView(micropixel::Scene& scene) {
        using namespace micropixel;
        browser_ = scene.CreateContainer().value();
        (void)browser_.CreateShape({0, 0, 720, 720}, kBackground).value();
        heading_clip_ = browser_.CreateContainer({.clip = {0, 0, 500, 48}, .translation = {24, 16}}).value();
        heading_ = heading_clip_.CreateLabel({0, 4}, "我的音乐", kText, SystemFont::kLarge).value();
        hint_ = browser_.CreateLabel({24, 68}, " ", kMuted, SystemFont::kSmall).value();
        auto button = [&](unsigned i, Rect bounds, const char* text, bool primary = false) {
            buttons_[i] = browser_.CreateTextButton({.bounds = bounds,
                                                     .text = text,
                                                     .style = {.background = primary ? kRed : kWhite,
                                                               .text = primary ? kWhite : kText,
                                                               .feedback = kMuted,
                                                               .font = SystemFont::kMedium,
                                                               .corner_radius = 14},
                                                     .overflow = ui::TextOverflow::kClip});
        };
        button(0, {616, 18, 80, 40}, "账户");
        button(1, {548, 18, 48, 40}, symbols::kRefresh);
        button(2, {24, 88, 536, 48}, "输入歌名或歌手");
        button(3, {576, 88, 120, 48}, "搜索", true);
        button(4, {24, 154, 672, 64}, "★ 我喜欢的音乐");
        button(5, {24, 102, 672, 64}, "播放全部", true);
        button(6, {24, 656, 208, 48}, "每日推荐");
        button(7, {256, 656, 208, 48}, "我的音乐");
        button(8, {488, 656, 208, 48}, "搜索");
        button(9, {24, 18, 48, 40}, symbols::kLeft);
        for (unsigned i = 0; i < 3; ++i)
            tab_marks_[i] = browser_.CreateShape({108 + int(i) * 232, 708, 40, 3}, kRed).value();
        profile_ = browser_.CreateContainer({.clip = {0, 0, 672, 68}, .translation = {24, 78}}).value();
        (void)profile_.CreateRoundedRect({0, 0, 64, 64}, {.fill = Color::Rgb(250, 224, 228), .radius = 20}).value();
        profile_placeholder_ = profile_.CreateLabel({32, 17}, "云", kRed, SystemFont::kLarge, true).value();
        profile_name_ = profile_.CreateLabel({84, 2}, " ", kText, SystemFont::kLarge).value();
        profile_signature_ = profile_.CreateLabel({84, 44}, " ", kMuted, SystemFont::kSmall).value();
        section_ = browser_.CreateLabel({24, 232}, "我的歌单", kText, SystemFont::kMedium).value();
        scroll_ = ui::ScrollView::CreateIn(browser_, {24, 268, 672, 280});
        for (auto& row : rows_)
            row = ui::ListItem::CreateIn(scroll_.content(), {0, 0, 664, 74},
                                         {.action_icon = "☆", .selected_action_icon = "★"});
        empty_ = browser_.CreateLabel({360, 340}, " ", kMuted, SystemFont::kMedium, true).value();
        notice_ = browser_.CreateLabel({360, 552}, " ", kMuted, SystemFont::kSmall, true).value();
        (void)browser_.CreateRoundedRect({24, 584, 672, 58}, {.fill = kWhite, .radius = 16}).value();
        (void)browser_.CreateRoundedRect({32, 592, 42, 42}, {.fill = Color::Rgb(250, 224, 228), .radius = 12}).value();
        mini_placeholder_ = browser_.CreateLabel({53, 601}, symbols::kAudio, kRed, SystemFont::kMedium, true).value();
        auto mini_text = browser_.CreateContainer({.clip = {0, 0, 404, 50}, .translation = {88, 588}}).value();
        mini_title_ = mini_text.CreateLabel({0, 2}, "还没有播放音乐", kText, SystemFont::kMedium).value();
        mini_artist_ = mini_text.CreateLabel({0, 31}, "选一首喜欢的歌", kMuted, SystemFont::kSmall).value();
        mini_body_ = ui::Button({24, 584, 468, 58});
        auto icon = [&](Container& parent, Rect bounds, const char* symbol, Color bg, Color fg) {
            return ui::IconButton::CreateIn(parent, {.bounds = bounds,
                                                     .icon = symbol,
                                                     .style = {.background = bg,
                                                               .text = fg,
                                                               .feedback = kMuted,
                                                               .font = SystemFont::kLarge,
                                                               .corner_radius = 48},
                                                     .selected_color = kRed});
        };
        mini_like_ = ui::IconButton::CreateIn(browser_, {.bounds = {500, 588, 48, 48},
                                                         .icon = "☆",
                                                         .selected_icon = "★",
                                                         .style = {.background = kWhite,
                                                                   .text = kMuted,
                                                                   .feedback = kWhite,
                                                                   .font = SystemFont::kLarge,
                                                                   .corner_radius = 24},
                                                         .selected_color = kRed});
        mini_play_ = icon(browser_, {556, 588, 56, 48}, symbols::kPlay, kWhite, kText);
        mini_next_ = icon(browser_, {624, 588, 56, 48}, symbols::kNext, kWhite, kText);

        player_ = scene.CreateContainer({.z_order = 20, .visible = false}).value();
        (void)player_.CreateShape({0, 0, 720, 720}, Color::Rgb(23, 30, 42)).value();
        (void)player_.CreateLabel({360, 23}, "正在播放", kWhite, SystemFont::kMedium, true).value();
        player_back_ = icon(player_, {24, 16, 56, 48}, symbols::kDown, Color::Rgb(23, 30, 42), kWhite);
        player_stop_ = icon(player_, {624, 16, 56, 48}, symbols::kStop, Color::Rgb(23, 30, 42), kWhite);
        cover_placeholder_ = player_.CreateContainer().value();
        (void)cover_placeholder_.CreateRoundedRect({248, 96, 224, 224}, {.fill = Color::Rgb(42, 51, 66), .radius = 112})
            .value();
        (void)cover_placeholder_.CreateRoundedRect({296, 144, 128, 128}, {.fill = Color::Rgb(68, 77, 91), .radius = 64})
            .value();
        (void)cover_placeholder_.CreateRoundedRect({332, 180, 56, 56}, {.fill = kRed, .radius = 28}).value();
        (void)cover_placeholder_.CreateLabel({360, 190}, symbols::kAudio, kWhite, SystemFont::kLarge, true).value();
        player_title_ = player_.CreateLabel({360, 342}, "请选择歌曲", kWhite, SystemFont::kLarge, true).value();
        player_artist_ =
            player_.CreateLabel({360, 384}, " ", Color::Rgb(179, 187, 201), SystemFont::kMedium, true).value();
        player_like_ = ui::IconButton::CreateIn(player_, {.bounds = {332, 422, 56, 40},
                                                          .icon = "☆",
                                                          .selected_icon = "★",
                                                          .style = {.background = Color::Rgb(23, 30, 42),
                                                                    .text = Color::Rgb(179, 187, 201),
                                                                    .feedback = kMuted,
                                                                    .font = SystemFont::kLarge,
                                                                    .corner_radius = 20},
                                                          .selected_color = kRed});
        seek_ = ui::Slider::CreateIn(player_, {.bounds = {40, 472, 640, 32},
                                               .maximum = 0,
                                               .track = Color::Rgb(73, 83, 99),
                                               .fill = kWhite,
                                               .thumb = kWhite});
        elapsed_ = player_.CreateLabel({48, 510}, "0:00", Color::Rgb(179, 187, 201), SystemFont::kSmall).value();
        duration_ =
            player_.CreateLabel({672, 510}, "0:00", Color::Rgb(179, 187, 201), SystemFont::kSmall, true).value();
        previous_ = icon(player_, {176, 556, 80, 80}, symbols::kPrevious, Color::Rgb(23, 30, 42), kWhite);
        play_ = icon(player_, {312, 548, 96, 96}, symbols::kPlay, kRed, kWhite);
        next_ = icon(player_, {464, 556, 80, 80}, symbols::kNext, Color::Rgb(23, 30, 42), kWhite);
        player_notice_ =
            player_.CreateLabel({360, 668}, " ", Color::Rgb(179, 187, 201), SystemFont::kSmall, true).value();
    }
    void ClearImages() {
        ClearCover();
        if (avatar_node_.valid()) avatar_node_.Destroy();
        avatar_ = micropixel::Texture{};
        profile_placeholder_.SetVisible(true);
    }
    void ClearCover() {
        if (cover_node_.valid()) cover_node_.Destroy();
        if (mini_cover_.valid()) mini_cover_.Destroy();
        cover_ = micropixel::Texture{};
        cover_placeholder_.SetVisible(true);
        mini_placeholder_.SetVisible(true);
    }
    bool SetAvatar(micropixel::Application& app, std::span<const uint8_t> bytes) {
        auto image = app.resources().DecodeTexture(bytes);
        if (!image) return false;
        if (avatar_node_.valid()) avatar_node_.Destroy();
        avatar_ = std::move(*image);
        avatar_node_ =
            profile_.CreateSprite(avatar_, {0, 0, 64, 64}, {0, 0, int32_t(avatar_.width()), int32_t(avatar_.height())})
                .value();
        profile_placeholder_.SetVisible(false);
        return true;
    }
    bool SetCover(micropixel::Application& app, std::span<const uint8_t> bytes) {
        auto image = app.resources().DecodeTexture(bytes);
        if (!image) return false;
        ClearCover();
        cover_ = std::move(*image);
        const micropixel::Rect source{0, 0, int32_t(cover_.width()), int32_t(cover_.height())};
        cover_node_ = player_.CreateSprite(cover_, {248, 96, 224, 224}, source).value();
        mini_cover_ = browser_.CreateSprite(cover_, {32, 592, 42, 42}, source).value();
        cover_placeholder_.SetVisible(false);
        mini_placeholder_.SetVisible(false);
        return true;
    }
    void Update(const Account& account, const Client& client, const MusicState& state, const Track& current,
                const micropixel::MediaState& media, bool playback) {
        using namespace micropixel;
        state_ = state;
        playing_ = playback && media.phase == MediaPhase::kPlaying;
        const bool full = state.screen == MusicScreen::kPlayer;
        browser_.SetVisible(!full);
        player_.SetVisible(full);
        if (!full) {
            const bool library = state.screen == MusicScreen::kLibrary;
            const bool searching = state.screen == MusicScreen::kSearch;
            const bool collection = state.screen == MusicScreen::kCollection;
            const auto heading = library      ? "我的音乐"
                                 : searching  ? "搜索音乐"
                                 : collection ? state.collection.c_str()
                                              : "每日推荐";
            heading_.SetText(Clip(heading, 54).c_str());
            heading_clip_.SetTranslation({collection ? 88 : 24, 16});
            heading_clip_.SetClip({0, 0, collection ? 440 : 500, 48});
            hint_.SetText(library      ? " "
                          : searching  ? "歌名、歌手，都可以搜"
                          : collection ? "滑动选曲，红星加入我喜欢的音乐"
                                       : "根据你的喜好，为今天选曲");
            buttons_[0].SetText(account.verified() ? "账户" : "登录").value();
            buttons_[9].SetVisible(collection);
            buttons_[2].SetVisible(searching);
            buttons_[3].SetVisible(searching);
            buttons_[2].SetText(Clip(state.query.empty() ? "输入歌名或歌手" : state.query, 72).c_str()).value();
            buttons_[4].SetVisible(library);
            buttons_[4]
                .SetText(
                    ("★ 我喜欢的音乐 · " + (client.likes_ready ? Decimal(client.liked_ids.size()) : "待同步") + " 首")
                        .c_str())
                .value();
            buttons_[5].SetVisible(!library && !searching);
            buttons_[5].SetText(account.verified() || collection ? "播放全部" : "登录后查看每日推荐").value();
            profile_.SetVisible(library);
            section_.SetVisible(library);
            profile_name_.SetText(Clip(account.verified() ? account.nickname : "登录网易云音乐", 54).c_str());
            profile_signature_.SetText(
                Clip(account.signature.empty() ? "让音乐陪伴每一天" : account.signature, 96).c_str());
            scroll_.SetBounds(library     ? Rect{24, 268, 672, 280}
                              : searching ? Rect{24, 156, 672, 392}
                                          : Rect{24, 192, 672, 356});
            const auto count =
                state.content == MusicContent::kPlaylists ? client.playlists.size() : client.tracks.size();
            if (last_revision_ != state.revision) {
                last_revision_ = state.revision;
                scroll_.Cancel();
                scroll_.ScrollTo(0);
                ResetRows();
            }
            scroll_.SetContentHeight(int32_t(count * 74));
            RenderRows(client, state, current.id);
            empty_.SetVisible(count == 0);
            empty_.SetText(state.loading                                                   ? "正在加载…"
                           : state.failed                                                  ? "加载失败，点击右上角刷新"
                           : !account.verified() && state.content != MusicContent::kSearch ? "登录后同步你的音乐"
                                                                                           : "这里还没有音乐");
            const auto& hint = state.load_hint.empty() ? state.notice : state.load_hint;
            notice_.SetText(Clip(hint.empty() ? " " : hint, 66).c_str());
            notice_.SetColor(state.failed ? kRed : kMuted);
            for (unsigned i = 0; i < 3; ++i) {
                bool selected = i == 0   ? state.screen == MusicScreen::kDiscover
                                : i == 1 ? (library || collection)
                                         : searching;
                auto style = buttons_[6 + i].style();
                if (style.text != (selected ? kRed : kText)) {
                    style.text = selected ? kRed : kText;
                    buttons_[6 + i].SetStyle(style).value();
                }
                tab_marks_[i].SetVisible(selected);
            }
        }
        mini_title_.SetText(Clip(!current.title.empty() ? current.title
                                 : current.id           ? "正在加载歌曲"
                                                        : "还没有播放音乐",
                                 54)
                                .c_str());
        mini_artist_.SetText(Clip(!current.artists.empty() ? current.artists
                                  : current.id             ? "未知歌手"
                                                           : "选一首喜欢的歌",
                                  84)
                                 .c_str());
        mini_play_.SetIcon(playing_ ? symbols::kPause : symbols::kPlay).value();
        mini_play_.SetEnabled(current.id != 0);
        mini_next_.SetEnabled(current.id != 0);
        for (auto* favorite : {&mini_like_, &player_like_}) {
            favorite->SetSelected(client.Liked(current.id));
            favorite->SetEnabled(current.id && (!account.verified() || client.likes_ready) && !state.favorite_pending);
            favorite->SetPending(state.favorite_pending == current.id && current.id);
        }
        player_title_.SetText(Clip(!current.title.empty() ? current.title
                                   : current.id           ? "正在加载歌曲"
                                                          : "请选择歌曲",
                                   54)
                                  .c_str());
        player_artist_.SetText(Clip(current.artists.empty() ? " " : current.artists, 72).c_str());
        play_.SetIcon(playing_ ? symbols::kPause : symbols::kPlay).value();
        play_.SetEnabled(current.id != 0);
        previous_.SetEnabled(current.id != 0);
        next_.SetEnabled(current.id != 0);
        seek_.SetVisible(full);
        seek_.SetMaximum(media.duration_ms);
        seek_.SetValue(media.position_ms);
        seek_.SetEnabled(playback && media.phase != MediaPhase::kLoading && media.phase != MediaPhase::kFailed);
        elapsed_.SetText(TimeText(seek_.dragging() ? seek_.value() : media.position_ms).c_str());
        duration_.SetText(TimeText(media.duration_ms).c_str());
        player_notice_.SetText(Clip(state.notice.empty() ? "顺序播放" : state.notice, 108).c_str());
    }
    MusicAction Touch(const micropixel::TouchEvent& event, const Client& client, const Track& current, bool& changed) {
        using namespace micropixel;
        MusicAction result;
        auto icon = [&](ui::IconButton& button, MusicActionKind action) {
            auto update = button.OnTouch(event);
            changed |= update.redraw();
            if (update.clicked) result.kind = action;
        };
        if (state_.screen == MusicScreen::kPlayer) {
            icon(player_back_, MusicActionKind::kBack);
            icon(player_stop_, MusicActionKind::kStop);
            icon(previous_, MusicActionKind::kPrevious);
            icon(play_, MusicActionKind::kToggle);
            icon(next_, MusicActionKind::kNext);
            icon(player_like_, MusicActionKind::kFavorite);
            auto seek = seek_.OnTouch(event);
            changed |= seek.changed;
            if (seek.committed) result = {MusicActionKind::kSeek, 0, seek_.value()};
            return result;
        }
        const std::array<MusicActionKind, 10> actions{MusicActionKind::kAccount,   MusicActionKind::kRefresh,
                                                      MusicActionKind::kEditQuery, MusicActionKind::kSubmit,
                                                      MusicActionKind::kLiked,     MusicActionKind::kPlayAll,
                                                      MusicActionKind::kDiscover,  MusicActionKind::kLibrary,
                                                      MusicActionKind::kSearch,    MusicActionKind::kBack};
        for (unsigned i = 0; i < buttons_.size(); ++i) {
            if (!buttons_[i].visible()) continue;
            auto update = buttons_[i].OnTouch(event);
            changed |= update.redraw();
            if (update.clicked) result.kind = actions[i];
        }
        icon(mini_like_, MusicActionKind::kFavorite);
        icon(mini_play_, MusicActionKind::kToggle);
        icon(mini_next_, MusicActionKind::kNext);
        auto body = mini_body_.OnTouch(event);
        if (body.clicked && current.id) result.kind = MusicActionKind::kOpenPlayer;
        if (event.phase() == TouchPhase::kDown && scroll_.contains(event.position())) load_fired_ = false;
        auto scrolling = scroll_.OnTouch(event);
        changed |= scrolling.changed;
        if (scrolling.dragging) {
            ResetRows();
            RenderRows(client, state_, current.id);
            if (event.phase() == TouchPhase::kUp && scroll_.at_end() && state_.has_more && !state_.loading &&
                !load_fired_) {
                load_fired_ = true;
                return {MusicActionKind::kLoadMore, 0, uint32_t(scroll_.offset())};
            }
            return result;
        }
        if (event.phase() == TouchPhase::kDown && !scroll_.contains(event.position())) return result;
        for (unsigned i = 0; i < rows_.size(); ++i) {
            auto update = rows_[i].OnTouch(event);
            changed |= update.redraw;
            if (update.clicked) result = {MusicActionKind::kRow, uint32_t(row_indices_[i]), 0};
            if (update.action_clicked) result = {MusicActionKind::kFavorite, uint32_t(row_indices_[i]), 1};
        }
        return result;
    }

   private:
    void ResetRows() {
        for (auto& row : rows_) row.Reset();
    }
    void RenderRows(const Client& client, const MusicState& state, uint64_t current) {
        const bool lists = state.content == MusicContent::kPlaylists;
        const auto count = lists ? client.playlists.size() : client.tracks.size();
        const uint32_t first = uint32_t(scroll_.offset() / 74);
        for (unsigned i = 0; i < rows_.size(); ++i) {
            const auto index = first + i;
            row_indices_[i] = index;
            rows_[i].SetVisible(index < count);
            if (index >= count) continue;
            rows_[i].SetPosition({0, int32_t(index * 74)});
            rows_[i].SetEnabled(!state.loading || state.appending);
            if (lists) {
                const auto& list = client.playlists[index];
                rows_[i]
                    .SetText(Clip(list.name, 112).c_str(),
                             (Decimal(list.track_count) + " 首 · " + (list.created ? "我创建的" : "我收藏的")).c_str(),
                             micropixel::symbols::kList)
                    .value();
                rows_[i].SetSelected(false);
                rows_[i].SetAction(false, false, false);
            } else {
                const auto& track = client.tracks[index];
                rows_[i]
                    .SetText(Clip(track.title, 112).c_str(), Clip(track.artists, 112).c_str(),
                             track.id == current ? micropixel::symbols::kAudio : Decimal(index + 1).c_str())
                    .value();
                rows_[i].SetSelected(track.id == current);
                rows_[i].SetAction(true, client.Liked(track.id), (!state.favorite_pending && client.likes_ready),
                                   state.favorite_pending == track.id);
            }
        }
    }
    static constexpr micropixel::Color kBackground = micropixel::Color::Rgb(247, 248, 250),
                                       kWhite = micropixel::Color::White(), kText = micropixel::Color::Rgb(28, 34, 43),
                                       kMuted = micropixel::Color::Rgb(124, 132, 144),
                                       kRed = micropixel::Color::Rgb(218, 55, 73);
    micropixel::ContainerNode browser_, player_, profile_, cover_placeholder_, heading_clip_;
    micropixel::LabelNode heading_, hint_, profile_name_, profile_signature_, profile_placeholder_, section_, empty_,
        notice_, mini_title_, mini_artist_, mini_placeholder_, player_title_, player_artist_, elapsed_, duration_,
        player_notice_;
    std::array<micropixel::ui::TextButton, 10> buttons_;
    std::array<micropixel::ShapeNode, 3> tab_marks_;
    std::array<micropixel::ui::ListItem, 8> rows_;
    std::array<uint32_t, 8> row_indices_{};
    micropixel::ui::ScrollView scroll_;
    micropixel::ui::Slider seek_;
    micropixel::ui::Button mini_body_;
    micropixel::ui::IconButton mini_like_, mini_play_, mini_next_, player_back_, player_stop_, previous_, play_, next_,
        player_like_;
    micropixel::Texture avatar_, cover_;
    micropixel::SpriteNode avatar_node_, cover_node_, mini_cover_;
    MusicState state_;
    uint32_t last_revision_{UINT32_MAX};
    bool playing_{}, load_fired_{};
};
}  // namespace netease
