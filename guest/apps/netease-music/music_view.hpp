// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>

#include "client.hpp"
#include "music_layout.hpp"
#include "music_state.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/panel.hpp"
#include "view_text.hpp"

namespace netease {
inline std::string TimeText(uint32_t ms) {
    auto s = ms / 1000;
    return Decimal(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + Decimal(s % 60);
}
class MusicView final {
   public:
    void CancelInput() {
        for (auto& button : buttons_) button.Reset();
        header_.CancelInput();
        navigation_.CancelInput();
        list_.CancelInput();
        seek_.Cancel();
        mini_body_.Reset();
        for (auto* button : {&mini_like_, &mini_play_, &mini_next_, &player_back_, &player_stop_, &previous_, &play_,
                             &next_, &player_like_, &player_list_})
            button->Reset();
    }
    MusicView(micropixel::Scene& scene, MusicLayout layout) : layout_(layout) {
        using namespace micropixel;
        const bool compact = layout.compact();
        const int32_t mini_top = layout.mini_top();
        browser_ = scene.CreateContainer().value();
        (void)browser_.CreateShape({0, 0, layout.width, layout.height}, kBackground).value();
        ui::Theme theme = ui::kPanelTheme;
        theme.radius = compact ? 10 : 14;
        const ui::AppBarAction header_actions[] = {{.text = symbols::kRefresh, .width = compact ? 44 : 48},
                                                   {.text = "账户", .width = compact ? 62 : 80}};
        header_ = ui::AppBar::Create(browser_,
                                     {.bounds = layout.box({16, 10, 688, 56}, {12, 4, 456, 46}),
                                      .title = "我的音乐",
                                      .theme = theme,
                                      .leading = ui::AppBarAction{.text = symbols::kLeft, .width = compact ? 42 : 48},
                                      .actions = header_actions,
                                      .padding = compact ? 0 : 8,
                                      .gap = compact ? 8 : 20,
                                      .action_height = compact ? 44 : 40,
                                      .title_line_height = compact ? 26 : 36,
                                      .surface = !compact})
                      .value();
        hint_ = browser_.CreateLabel({24, 134}, " ", kMuted, SystemFont::kSmall).value();
        hint_.SetVisible(!compact);
        auto button = [&](unsigned i, Rect bounds, const char* text, bool primary = false) {
            buttons_[i] = browser_.CreateTextButton({.bounds = bounds,
                                                     .text = text,
                                                     .style = {.background = primary ? kAccent : kSurface,
                                                               .text = primary ? kBackground : kText,
                                                               .feedback = kMuted,
                                                               .font = SystemFont::kMedium,
                                                               .corner_radius = theme.radius},
                                                     .overflow = ui::TextOverflow::kClip});
        };
        button(0, layout.box({24, 146, 536, 48}, {12, 104, 368, 44}), "输入歌名或歌手");
        button(1, layout.box({576, 146, 120, 48}, {388, 104, 80, 44}), "搜索", true);
        button(2, layout.box({24, 220, 672, 54}, {12, 156, 456, 44}), "★ 我喜欢的音乐");
        button(3, layout.box({24, 156, 672, 56}, {12, 104, 456, 44}), "播放全部", true);
        const char* navigation_labels[] = {"每日推荐", "我的音乐", "搜索"};
        auto normal = theme.ButtonStyle();
        if (compact) normal.background = kBackground;
        auto selected = normal;
        selected.text = kAccent;
        navigation_ =
            ui::SegmentedControl<3>::Create(
                browser_, layout.box({24, 74, 672, 54}, {12, 52, 456, 44}), navigation_labels,
                {.normal = normal, .selected = selected, .gap = compact ? 4 : 24, .underline = true, .mark = kAccent})
                .value();
        profile_ = browser_
                       .CreateContainer({.clip = layout.box({0, 0, 672, 68}, {0, 0, 456, 48}),
                                         .translation = layout.point({24, 142}, {12, 102})})
                       .value();
        (void)profile_.CreateRoundedRect(AvatarBounds(), {.fill = ui::panel::Raised, .radius = compact ? 12U : 20U})
            .value();
        profile_placeholder_ =
            profile_.CreateLabel(layout.point({32, 17}, {22, 10}), "云", kRed, SystemFont::kLarge, true).value();
        profile_name_ = profile_.CreateLabel(layout.point({84, 2}, {58, 12}), " ", kText, SystemFont::kLarge).value();
        profile_signature_ = profile_.CreateLabel({84, 44}, " ", kMuted, SystemFont::kSmall).value();
        profile_signature_.SetVisible(!compact);
        section_ =
            browser_.CreateLabel(layout.point({24, 284}, {12, 208}), "我的歌单", kText, SystemFont::kMedium).value();
        list_ = ui::VirtualList<ui::ListItem, 8>::Create(
                    browser_,
                    {.bounds = ListBounds(MusicScreen::kLibrary),
                     .item_width = compact ? 450 : 664,
                     .item_height = compact ? kCompactRowHeight : 74},
                    [compact](Container& content, Rect bounds) {
                        return content.CreateListItem(bounds, {.action_icon = "☆",
                                                               .selected_action_icon = "★",
                                                               .background = compact ? kBackground : kSurface,
                                                               .title = kText,
                                                               .subtitle = kMuted,
                                                               .action = kMuted,
                                                               .selected = kAccent,
                                                               .action_selected = kRed,
                                                               .divider = ui::panel::Line});
                    })
                    .value();
        empty_ = ui::ContentStateView::Create(browser_, layout.box({24, 340, 672, 100}, {12, 258, 456, 60})).value();
        notice_ =
            browser_.CreateLabel(layout.point({360, 622}, {240, mini_top - 26}), " ", kMuted, SystemFont::kSmall, true)
                .value();
        (void)browser_
            .CreateRoundedRect(layout.box({24, mini_top, 672, 62}, {12, mini_top, 456, 56}),
                               {.fill = kSurface, .radius = compact ? 12U : 16U})
            .value();
        (void)browser_
            .CreateRoundedRect(layout.mini_cover(), {.fill = ui::panel::Raised, .radius = compact ? 10U : 12U})
            .value();
        mini_placeholder_ = browser_
                                .CreateLabel(layout.point({53, mini_top + 17}, {39, mini_top + 17}), symbols::kAudio,
                                             kRed, SystemFont::kMedium, true)
                                .value();
        auto mini_text = browser_
                             .CreateContainer({.clip = layout.box({0, 0, 404, 60}, {0, 0, 296, 56}),
                                               .translation = layout.point({88, mini_top + 1}, {68, mini_top})})
                             .value();
        mini_title_ =
            mini_text.CreateLabel(layout.point({0, 2}, {0, 6}), "还没有播放音乐", kText, SystemFont::kMedium).value();
        mini_artist_ = mini_text.CreateLabel(layout.point({0, 31}, {0, 31}), " ", kMuted, SystemFont::kSmall).value();
        mini_body_ = ui::Button(layout.box({24, mini_top, 468, 62}, {12, mini_top, 350, 56}));
        auto icon = [&](Container& parent, Rect bounds, const char* symbol, Color bg, Color fg) {
            return ui::IconButton::CreateIn(
                parent, {.bounds = bounds,
                         .icon = symbol,
                         .style = {.background = bg,
                                   .text = fg,
                                   .feedback = kMuted,
                                   .font = SystemFont::kLarge,
                                   .corner_radius = uint32_t(std::min(bounds.width, bounds.height) / 2)},
                         .selected_color = kRed});
        };
        mini_like_ = ui::IconButton::CreateIn(
            browser_, {.bounds = layout.box({500, mini_top + 7, 48, 48}, {318, mini_top + 6, 44, 44}),
                       .icon = "☆",
                       .selected_icon = "★",
                       .style = {.background = kSurface,
                                 .text = kMuted,
                                 .feedback = kWhite,
                                 .font = SystemFont::kLarge,
                                 .corner_radius = 24},
                       .selected_color = kRed});
        mini_like_.SetVisible(!compact);
        mini_play_ = icon(browser_, layout.box({556, mini_top + 7, 56, 48}, {366, mini_top + 6, 44, 44}),
                          symbols::kPlay, compact ? kAccent : kSurface, compact ? kBackground : kText);
        mini_next_ = icon(browser_, layout.box({624, mini_top + 7, 56, 48}, {416, mini_top + 6, 44, 44}),
                          symbols::kNext, kSurface, kText);

        player_ = scene.CreateContainer({.z_order = 20, .visible = false}).value();
        if (compact) {
            (void)player_.CreateShape({0, 0, layout.width, layout.height}, kBackground).value();
        } else {
            for (unsigned i = 0; i < 30; ++i)
                (void)player_
                    .CreateShape({int(i) * 24, 0, 24, 720},
                                 Color::Rgb(58 - 40 * i / 29, 41 - 16 * i / 29, 35 + 3 * i / 29))
                    .value();
        }
        (void)player_.CreateLabel(layout.point({360, 24}, {240, 16}), "网易云音乐", kMuted, SystemFont::kMedium, true)
            .value();
        player_back_ =
            icon(player_, layout.box({24, 16, 56, 48}, {12, 4, 48, 46}), symbols::kDown, kBackground, kWhite);
        player_stop_ =
            icon(player_, layout.box({624, 16, 56, 48}, {420, 4, 48, 46}), symbols::kStop, kBackground, kWhite);
        cover_placeholder_ = player_.CreateContainer().value();
        (void)cover_placeholder_
            .CreateRoundedRect(layout.player_cover(), {.fill = kSurface, .radius = compact ? 14U : 52U})
            .value();
        (void)cover_placeholder_
            .CreateRoundedRect(layout.box({78, 142, 44, 44}, {36, 86, 44, 44}), {.fill = kAccent, .radius = 22})
            .value();
        (void)cover_placeholder_
            .CreateLabel(layout.point({100, 150}, {58, 95}), symbols::kAudio, kWhite, SystemFont::kMedium, true)
            .value();
        auto track_text = player_
                              .CreateContainer({.clip = layout.box({0, 0, 508, 112}, {0, 0, 354, 80}),
                                                .translation = layout.point({176, 112}, {114, 72})})
                              .value();
        player_title_ = track_text.CreateLabel({0, 2}, "请选择歌曲", kWhite, SystemFont::kTitle).value();
        player_artist_ = track_text
                             .CreateLabel(layout.point({0, 58}, {0, 40}), " ", kMuted,
                                          compact ? SystemFont::kSmall : SystemFont::kLarge)
                             .value();
        previous_ =
            icon(player_, layout.box({152, 286, 80, 80}, {100, 176, 56, 56}), symbols::kPrevious, kBackground, kWhite);
        play_ = icon(player_, layout.box({296, 262, 128, 128}, {202, 166, 76, 76}), symbols::kPlay, kAccent,
                     compact ? kBackground : kWhite);
        next_ = icon(player_, layout.box({488, 286, 80, 80}, {324, 176, 56, 56}), symbols::kNext, kBackground, kWhite);
        player_like_ = ui::IconButton::CreateIn(player_, {.bounds = layout.box({96, 448, 72, 64}, {28, 250, 48, 46}),
                                                          .icon = "☆",
                                                          .selected_icon = "★",
                                                          .style = {.background = kBackground,
                                                                    .text = kWhite,
                                                                    .feedback = kMuted,
                                                                    .font = SystemFont::kTitle,
                                                                    .corner_radius = 32},
                                                          .selected_color = kRed});
        if (!compact) (void)player_.CreateLabel({360, 466}, "顺序播放", kMuted, SystemFont::kMedium, true).value();
        player_list_ =
            icon(player_, layout.box({552, 448, 72, 64}, {404, 250, 48, 46}), symbols::kList, kBackground, kWhite);
        seek_ = ui::Slider::CreateIn(player_, {.bounds = layout.box({48, 566, 624, 32}, {28, 308, 424, 32}),
                                               .maximum = 0,
                                               .track = ui::panel::Line,
                                               .fill = kWhite,
                                               .thumb = kWhite});
        elapsed_ = player_.CreateLabel(layout.point({56, 610}, {28, 350}), "0:00", kMuted, SystemFont::kSmall).value();
        duration_ =
            player_.CreateLabel(layout.point({664, 610}, {425, 350}), "0:00", kMuted, SystemFont::kSmall, true).value();
        player_notice_ =
            player_
                .CreateLabel(layout.point({360, 676}, {240, layout.height - 34}), " ", kMuted, SystemFont::kSmall, true)
                .value();
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
            profile_.CreateSprite(avatar_, AvatarBounds(), {0, 0, int32_t(avatar_.width()), int32_t(avatar_.height())})
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
        cover_node_ = player_.CreateSprite(cover_, layout_.player_cover(), source).value();
        mini_cover_ = browser_.CreateSprite(cover_, layout_.mini_cover(), source).value();
        cover_placeholder_.SetVisible(false);
        mini_placeholder_.SetVisible(false);
        return true;
    }
    void Update(const Account& account, const Client& client, const MusicState& state, const Track& current,
                const micropixel::MediaState& media, bool playback) {
        using namespace micropixel;
        if (state_.screen != state.screen) CancelInput();
        state_ = state;
        playing_ = playback && media.phase == MediaPhase::kPlaying;
        const bool full = state.screen == MusicScreen::kPlayer;
        browser_.SetVisible(!full);
        player_.SetVisible(full);
        if (!full) {
            const bool library = state.screen == MusicScreen::kLibrary;
            const bool searching = state.screen == MusicScreen::kSearch;
            const bool collection = state.screen == MusicScreen::kCollection;
            const auto heading = collection          ? state.collection.c_str()
                                 : layout_.compact() ? "网易云音乐"
                                 : library           ? "我的音乐"
                                 : searching         ? "搜索音乐"
                                                     : "每日推荐";
            header_.SetTitle(Clip(heading, 54).c_str()).value();
            header_.SetLeadingVisible(collection);
            hint_.SetText(library || searching ? " "
                          : collection         ? "滑动选曲，红星加入我喜欢的音乐"
                                               : "根据你的喜好，为今天选曲");
            header_.SetActionText(1, account.verified() ? "账户" : "登录").value();
            buttons_[0].SetVisible(searching);
            buttons_[1].SetVisible(searching);
            buttons_[0].SetText(Clip(state.query.empty() ? "输入歌名或歌手" : state.query, 72).c_str()).value();
            buttons_[2].SetVisible(library);
            buttons_[2]
                .SetText(
                    ("★ 我喜欢的音乐 · " + (client.likes_ready ? Decimal(client.liked_ids.size()) : "待同步") + " 首")
                        .c_str())
                .value();
            buttons_[3].SetVisible(!library && !searching);
            buttons_[3].SetText(account.verified() || collection ? "播放全部" : "登录后查看每日推荐").value();
            profile_.SetVisible(library);
            section_.SetVisible(library);
            profile_name_.SetText(Clip(account.verified() ? account.nickname : "登录网易云音乐", 54).c_str());
            profile_signature_.SetText(
                Clip(account.signature.empty() ? "让音乐陪伴每一天" : account.signature, 96).c_str());
            const auto count =
                state.content == MusicContent::kPlaylists ? client.playlists.size() : client.tracks.size();
            const bool show_notice = !layout_.compact() || (count != 0 && (state.loading || state.failed));
            list_.SetBounds(ListBounds(state.screen, show_notice)).value();
            const bool replaced = last_revision_ != state.revision;
            last_revision_ = state.revision;
            list_.SetItems(uint32_t(count), state.revision, replaced).value();
            RenderRows(client, state, current.id);
            empty_
                .Set({.phase = state.loading  ? ui::ContentPhase::kLoading
                               : state.failed ? ui::ContentPhase::kFailed
                                              : ui::ContentPhase::kReady,
                      .has_content = count != 0,
                      .message = state.loading  ? "正在加载…"
                                 : state.failed ? "加载失败，点击右上角刷新"
                                 : !account.verified() && state.content != MusicContent::kSearch ? "登录后同步你的音乐"
                                                                                                 : "这里还没有音乐"})
                .value();
            const auto& hint = state.load_hint.empty() ? state.notice : state.load_hint;
            notice_.SetText(Clip(hint.empty() || !show_notice ? " " : hint, 66).c_str());
            notice_.SetColor(state.failed ? kRed : kMuted);
            navigation_.SetSelected(library || collection ? 1 : searching ? 2 : 0);
        }
        mini_title_.SetText(Clip(!current.title.empty() ? current.title
                                 : current.id           ? "正在加载歌曲"
                                                        : "还没有播放音乐",
                                 54)
                                .c_str());
        if (layout_.compact()) mini_title_.SetPosition({0, current.id ? 6 : 17});
        mini_artist_.SetText(Clip(!current.artists.empty() ? current.artists
                                  : current.id             ? "未知歌手"
                                  : layout_.compact()      ? " "
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
        const auto default_notice = layout_.compact() ? " " : "顺序播放";
        player_notice_.SetText(Clip(state.notice.empty() ? default_notice : state.notice, 108).c_str());
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
            icon(player_list_, MusicActionKind::kBack);
            auto seek = seek_.OnTouch(event);
            changed |= seek.changed;
            if (seek.committed) result = {MusicActionKind::kSeek, 0, seek_.value()};
            return result;
        }
        auto header = header_.OnTouch(event);
        changed |= header.changed;
        if (header.action) {
            result.kind = header.action->kind == ui::AppBarActionKind::kLeading ? MusicActionKind::kBack
                          : header.action->index == 0                           ? MusicActionKind::kRefresh
                                                                                : MusicActionKind::kAccount;
            return result;
        }
        if (header.handled) return result;
        auto navigation = navigation_.OnTouch(event);
        changed |= navigation.changed;
        if (navigation.action) {
            const MusicActionKind destinations[] = {MusicActionKind::kDiscover, MusicActionKind::kLibrary,
                                                    MusicActionKind::kSearch};
            result.kind = destinations[*navigation.action];
            return result;
        }
        if (navigation.handled) return result;
        const std::array<MusicActionKind, 4> actions{MusicActionKind::kEditQuery, MusicActionKind::kSubmit,
                                                     MusicActionKind::kLiked, MusicActionKind::kPlayAll};
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
        if (event.phase() == TouchPhase::kDown && list_.contains(event.position())) load_fired_ = false;
        auto update = list_.OnTouch(
            event, [&](uint32_t index) { return KeyAt(client, state_, index); },
            [&](auto& row, uint32_t index, Rect bounds) { BindRow(row, client, state_, current.id, index, bounds); },
            [](auto& row, const auto& touch) {
                auto update = row.OnTouch(touch);
                return ui::RowUpdate{update.redraw, update.clicked, update.action_clicked};
            });
        changed |= update.changed;
        if (update.drag_ended && list_.at_end() && state_.has_more && !state_.loading && !load_fired_) {
            load_fired_ = true;
            return {MusicActionKind::kLoadMore, 0, uint32_t(list_.offset())};
        }
        if (update.action) {
            // Validate identity against the live dataset before translating to the App's index API.
            const auto& item = *update.action;
            if (item.generation == state_.revision && KeyAt(client, state_, item.index) == item.key)
                result = {item.region == ui::ItemRegion::kTrailing ? MusicActionKind::kFavorite : MusicActionKind::kRow,
                          item.index, item.region == ui::ItemRegion::kTrailing ? 1U : 0U};
        }
        return result;
    }

   private:
    static constexpr micropixel::Rect kLibraryList{24, 316, 672, 300};
    static constexpr micropixel::Rect kSearchList{24, 208, 672, 408};
    static constexpr micropixel::Rect kTrackList{24, 224, 672, 392};
    static constexpr int32_t kCompactRowHeight = 64;

    micropixel::Rect AvatarBounds() const { return layout_.box({0, 0, 64, 64}, {0, 0, 44, 44}); }

    micropixel::Rect ListBounds(MusicScreen screen, bool reserve_notice = false) const {
        const bool library = screen == MusicScreen::kLibrary;
        const int32_t top = library ? 234 : 156;
        const int32_t available = layout_.mini_top() - (reserve_notice ? 32 : 4) - top;
        const int32_t height = reserve_notice ? available : available / kCompactRowHeight * kCompactRowHeight;
        return layout_.box(library                          ? kLibraryList
                           : screen == MusicScreen::kSearch ? kSearchList
                                                            : kTrackList,
                           {12, top, 456, height});
    }

    static uint64_t KeyAt(const Client& client, const MusicState& state, uint32_t index) {
        return state.content == MusicContent::kPlaylists ? client.playlists[index].id : client.tracks[index].id;
    }
    void BindRow(micropixel::ui::ListItem& row, const Client& client, const MusicState& state, uint64_t current,
                 uint32_t index, micropixel::Rect bounds) {
        row.SetPosition({bounds.x, bounds.y});
        row.SetEnabled(!state.loading || state.appending);
        if (state.content == MusicContent::kPlaylists) {
            const auto& list = client.playlists[index];
            row.SetText(Clip(list.name, 112).c_str(),
                        (Decimal(list.track_count) + " 首 · " + (list.created ? "我创建的" : "我收藏的")).c_str(),
                        micropixel::symbols::kList)
                .value();
            row.SetSelected(false);
            row.SetAction(false, false, false);
        } else {
            const auto& track = client.tracks[index];
            row.SetText(Clip(track.title, 112).c_str(), Clip(track.artists, 112).c_str(),
                        track.id == current ? micropixel::symbols::kAudio : Decimal(index + 1).c_str())
                .value();
            row.SetSelected(track.id == current);
            row.SetAction(true, client.Liked(track.id), (!state.favorite_pending && client.likes_ready),
                          state.favorite_pending == track.id);
        }
    }
    void RenderRows(const Client& client, const MusicState& state, uint64_t current) {
        list_.BindRows([&](uint32_t index) { return KeyAt(client, state, index); },
                       [&](auto& row, uint32_t index, micropixel::Rect bounds) {
                           BindRow(row, client, state, current, index, bounds);
                       },
                       true);
    }
    static constexpr micropixel::Color kBackground = micropixel::ui::panel::Background,
                                       kSurface = micropixel::ui::panel::Surface, kWhite = micropixel::ui::panel::Text,
                                       kText = micropixel::ui::panel::Text, kMuted = micropixel::ui::panel::Muted,
                                       kRed = micropixel::ui::panel::Red, kAccent = micropixel::ui::panel::Accent;
    MusicLayout layout_;
    micropixel::ContainerNode browser_, player_, profile_, cover_placeholder_;
    micropixel::LabelNode hint_, profile_name_, profile_signature_, profile_placeholder_, section_, notice_,
        mini_title_, mini_artist_, mini_placeholder_, player_title_, player_artist_, elapsed_, duration_,
        player_notice_;
    std::array<micropixel::ui::TextButton, 4> buttons_;
    micropixel::ui::AppBar header_;
    micropixel::ui::SegmentedControl<3> navigation_;
    micropixel::ui::VirtualList<micropixel::ui::ListItem, 8> list_;
    micropixel::ui::ContentStateView empty_;
    micropixel::ui::Slider seek_;
    micropixel::ui::Button mini_body_;
    micropixel::ui::IconButton mini_like_, mini_play_, mini_next_, player_back_, player_stop_, previous_, play_, next_,
        player_like_, player_list_;
    micropixel::Texture avatar_, cover_;
    micropixel::SpriteNode avatar_node_, cover_node_, mini_cover_;
    MusicState state_;
    uint32_t last_revision_{UINT32_MAX};
    bool playing_{}, load_fired_{};
};
}  // namespace netease
