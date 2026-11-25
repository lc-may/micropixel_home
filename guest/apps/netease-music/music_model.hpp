// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "client.hpp"
#include "login.hpp"
#include "music_state.hpp"
#include "sdk/micropixel.hpp"
#include "view_text.hpp"
namespace netease {
using namespace micropixel;
using namespace micropixel::literals;
class MusicModel final {
   public:
    explicit MusicModel(Application& app)
        : app_(app), account_(app), client_(app, account_), play_client_(app, account_), login_(app, account_) {}
    template <class F>
    decltype(auto) WithPlayback(F&& fn) {
        ComponentContext context(app_, service_token_);
        return context.WithScope(static_cast<F&&>(fn));
    }
    template <class DrawInitial>
    void Initialize(DrawInitial draw_initial) {
        if (const char* query = app_.launch_arguments().FindValue("--query")) {
            state_.query = Clip(query, 128);
            explicit_query_ = true;
        }
        if (const char* song = app_.launch_arguments().FindValue("--song")) {
            for (const char* p = song; *p; ++p) {
                if (*p < '0' || *p > '9' || initial_song_ > (9007199254740991ULL - uint64_t(*p - '0')) / 10) {
                    initial_song_ = 0;
                    break;
                }
                initial_song_ = initial_song_ * 10 + *p - '0';
            }
        }
        const bool open_login = app_.launch_arguments().HasFlag("--login");
        if (!open_login && !initial_song_) {
            state_.loading = true;
            draw_initial();
        }
        if (open_login)
            OpenAccount();
        else if (account_.HasToken())
            login_.Verify();
        else
            StartInitial();
        Draw();
        app_.log().Info("NETEASE_GUEST started: SDK Scene + Network HTTP + Audio URL");
        app_.log().Info("NETEASE_GUEST version: 0.8.0");
        initialized_ = true;
    }

   private:
    static bool ImageOperation(Operation op) { return op == Operation::kAvatar || op == Operation::kCover; }
    static bool BrowseOperation(Operation op) {
        return op == Operation::kSearch || op == Operation::kPlaylists || op == Operation::kRecommendations ||
               op == Operation::kPlaylistIndex || op == Operation::kPlaylistTracks;
    }
    void Message(std::string text, bool failed = false) {
        state_.notice = std::move(text);
        state_.failed = failed;
        app_.log().Info(state_.notice.c_str());
    }
    void Draw() { ++dirty_; }
    void StopEditing() {
        state_.editing = false;
        (void)app_.input().EnableText(false);
    }
    void StartInitial() {
        initial_started_ = true;
        if (initial_song_) {
            current_.id = initial_song_;
            play_stage_ = PlayStage::kDetail;
            state_.screen = MusicScreen::kPlayer;
            Message("正在读取歌曲…");
        } else
            Browse(account_.verified() && !explicit_query_ ? MusicContent::kPlaylists : MusicContent::kSearch);
        Pump();
    }
    void OpenAccount() {
        StopEditing();
        if (!client_.mutation_busy()) {
            if (BrowseOperation(client_.operation())) read_pending_ = true;
            client_.Cancel();
        }
        login_visible_ = true;
        if (account_.verified()) {
            login_.phase = LoginPhase::kAuthenticated;
            login_.message = "已登录网易云音乐";
        } else if (account_.HasToken())
            login_.Verify();
        else
            login_.Start();
    }
    void CloseAccount() {
        login_.Cancel();
        login_visible_ = false;
        if (!initial_started_) StartInitial();
    }
    void ClearPrivate() {
        client_.ClearLibrary();
        WithPlayback([&] {
            play_client_.Cancel();
            playback_.Close();
        });
        queue_.clear();
        queue_revision_ = UINT32_MAX;
        current_ = {};
        media_ = {};
        avatar_image_.clear();
        cover_image_.clear();
        ++avatar_revision_;
        ++cover_revision_;
        owner_ = 0;
        avatar_pending_ = cover_pending_ = false;
        read_pending_ = sync_likes_ = false;
        state_.favorite_pending = 0;
        favorite_issued_ = false;
        play_stage_ = PlayStage::kNone;
        state_ = MusicState{};
        state_.revision = ++revision_;
        StopEditing();
    }
    void Authenticated() {
        if (owner_ && owner_ != account_.user_id) ClearPrivate();
        client_.ClearLibrary();
        avatar_image_.clear();
        cover_image_.clear();
        ++avatar_revision_;
        ++cover_revision_;
        owner_ = account_.user_id;
        avatar_pending_ = true;
        sync_likes_ = true;
        login_visible_ = false;
        app_.log().Info("NETEASE_LOGIN authenticated");
        if (!initial_started_)
            StartInitial();
        else
            Browse(MusicContent::kPlaylists);
    }
    void Browse(MusicContent content, uint32_t batch = 0, bool reset = false) {
        StopEditing();
        if (!client_.mutation_busy()) client_.Cancel();
        state_.content = content;
        state_.batch = batch;
        state_.has_more = false;
        state_.appending = false;
        state_.load_hint.clear();
        client_.ClearBrowse();
        state_.revision = ++revision_;
        state_.screen = content == MusicContent::kPlaylists         ? MusicScreen::kLibrary
                        : content == MusicContent::kRecommendations ? MusicScreen::kDiscover
                        : content == MusicContent::kSearch          ? MusicScreen::kSearch
                                                                    : MusicScreen::kCollection;
        state_.loading = true;
        state_.failed = false;
        read_pending_ = true;
        reset_playlist_ = reset;
        Message(content == MusicContent::kPlaylists         ? "正在同步我的歌单…"
                : content == MusicContent::kRecommendations ? "正在读取每日推荐…"
                : content == MusicContent::kSearch          ? "正在搜索…"
                                                            : "正在读取歌曲…");
        Pump();
    }
    void Pump() {
        if (!activity_token_) return;
        ComponentContext context(app_, activity_token_);
        context.WithScope([&] { PumpPage(); });
    }
    void PumpPage() {
        if (client_.busy() || (login_visible_ && !account_.verified())) return;
        if (sync_likes_ && account_.verified()) {
            sync_likes_ = false;
            if (!client_.Likes()) Message(client_.error, true);
            return;
        }
        if (state_.favorite_pending && !favorite_issued_) {
            if (!account_.verified() || !client_.likes_ready) {
                state_.favorite_pending = 0;
                Message("收藏状态未同步，请刷新后重试", true);
                return;
            }
            if (client_.SetLike(state_.favorite_pending, favorite_value_))
                favorite_issued_ = true;
            else {
                state_.favorite_pending = 0;
                Message(client_.error, true);
            }
            return;
        }
        if (read_pending_) {
            read_pending_ = false;
            bool started = false;
            if (state_.content == MusicContent::kSearch)
                started = !state_.query.empty() && client_.Search(state_.query, state_.batch, state_.appending);
            else if (!account_.verified()) {
                state_.loading = false;
                client_.tracks.clear();
                client_.playlists.clear();
                Message("登录后同步你的音乐");
                return;
            } else if (state_.content == MusicContent::kPlaylists)
                started = client_.Playlists(state_.batch, state_.appending);
            else if (state_.content == MusicContent::kRecommendations)
                started = client_.Recommendations(state_.appending);
            else if (state_.content == MusicContent::kPlaylist)
                started = client_.PlaylistTracks(playlist_id_, state_.batch, reset_playlist_, state_.appending);
            else if (client_.liked_ids.empty()) {
                client_.tracks.clear();
                client_.total = 0;
                state_.loading = false;
                Message("还没有喜欢的音乐，点击红星即可加入");
                return;
            } else
                started = client_.LikedTracks(state_.batch, state_.appending);
            reset_playlist_ = false;
            if (!started) {
                state_.loading = false;
                Message(client_.error.empty() ? "输入歌名或歌手开始搜索" : client_.error, !state_.query.empty());
            }
            return;
        }
        if (cover_pending_) {
            cover_pending_ = false;
            cover_id_ = current_.id;
            (void)client_.Cover(current_);
            return;
        }
        if (avatar_pending_ && account_.verified()) {
            avatar_pending_ = false;
            (void)client_.Avatar();
        }
    }
    void StartTrack(Track track, bool open = true) {
        StopEditing();
        WithPlayback([&] {
            play_client_.Cancel();
            playback_.Close();
        });
        if (track.id != current_.id) cover_image_.clear();
        ++cover_revision_;
        current_ = std::move(track);
        media_ = {};
        last_phase_ = 0;
        cover_pending_ = !current_.cover_url.empty();
        play_stage_ = current_.cover_url.empty() ? PlayStage::kDetail : PlayStage::kResolve;
        if (open && state_.screen != MusicScreen::kPlayer) {
            return_screen_ = state_.screen;
            state_.screen = MusicScreen::kPlayer;
        }
        Message("正在获取播放地址…");
        if (service_token_) WithPlayback([&] { PumpPlayback(); });
        Pump();
    }
    void Step(int direction) {
        const auto next = selected_ + direction;
        if (next < 0 || size_t(next) >= queue_.size()) return;
        selected_ = next;
        StartTrack(queue_[selected_], state_.screen == MusicScreen::kPlayer);
    }
    void Favorite(uint64_t id) {
        if (!id || state_.favorite_pending) return;
        if (!account_.verified()) {
            OpenAccount();
            return;
        }
        if (!client_.likes_ready) {
            sync_likes_ = true;
            Message("正在同步收藏状态，请稍后再点红星");
            Pump();
            return;
        }
        state_.favorite_pending = id;
        favorite_value_ = !client_.Liked(id);
        favorite_issued_ = false;
        if (ImageOperation(client_.operation())) client_.Cancel();
        Pump();
    }
    void Act(const MusicAction& action) {
        switch (action.kind) {
            case MusicActionKind::kNone:
                return;
            case MusicActionKind::kLibrary:
                Browse(MusicContent::kPlaylists);
                break;
            case MusicActionKind::kDiscover:
                Browse(MusicContent::kRecommendations);
                break;
            case MusicActionKind::kSearch:
                Browse(MusicContent::kSearch);
                break;
            case MusicActionKind::kAccount:
                OpenAccount();
                break;
            case MusicActionKind::kRefresh:
                sync_likes_ = account_.verified();
                Browse(state_.content, 0, true);
                break;
            case MusicActionKind::kLiked:
                state_.collection = "我喜欢的音乐";
                if (!account_.verified())
                    OpenAccount();
                else
                    Browse(MusicContent::kLiked);
                break;
            case MusicActionKind::kLoadMore:
                LoadMore();
                break;
            case MusicActionKind::kRow:
                if (state_.content == MusicContent::kPlaylists) {
                    if (action.index < client_.playlists.size()) {
                        playlist_id_ = client_.playlists[action.index].id;
                        state_.collection = client_.playlists[action.index].name;
                        Browse(MusicContent::kPlaylist, 0, true);
                    }
                } else if (action.index < client_.tracks.size()) {
                    queue_ = client_.tracks;
                    queue_revision_ = state_.revision;
                    selected_ = action.index;
                    StartTrack(queue_[selected_]);
                }
                break;
            case MusicActionKind::kFavorite:
                Favorite(action.value == 1 && action.index < client_.tracks.size() ? client_.tracks[action.index].id
                                                                                   : current_.id);
                break;
            case MusicActionKind::kPlayAll:
                if (!account_.verified() && state_.content == MusicContent::kRecommendations)
                    OpenAccount();
                else if (!client_.tracks.empty() && !state_.loading) {
                    queue_ = client_.tracks;
                    queue_revision_ = state_.revision;
                    selected_ = 0;
                    StartTrack(queue_[0]);
                }
                break;
            case MusicActionKind::kOpenPlayer:
                return_screen_ = state_.screen;
                state_.screen = MusicScreen::kPlayer;
                break;
            case MusicActionKind::kBack:
                if (state_.screen == MusicScreen::kPlayer)
                    state_.screen = return_screen_;
                else
                    Browse(MusicContent::kPlaylists);
                break;
            case MusicActionKind::kPrevious:
                Step(-1);
                break;
            case MusicActionKind::kNext:
                Step(1);
                break;
            case MusicActionKind::kToggle:
                if (playback_.valid() && (media_.phase == MediaPhase::kPlaying || media_.phase == MediaPhase::kPaused))
                    WithPlayback([&] { (void)playback_.Pause(media_.phase == MediaPhase::kPlaying); });
                else if (!playback_.valid() && current_.id)
                    StartTrack(current_, state_.screen == MusicScreen::kPlayer);
                break;
            case MusicActionKind::kStop:
                if (client_.operation() == Operation::kResolve || client_.operation() == Operation::kDetail ||
                    client_.operation() == Operation::kCover)
                    client_.Cancel();
                cover_pending_ = false;
                WithPlayback([&] {
                    play_client_.Cancel();
                    playback_.Close();
                });
                media_ = {};
                play_stage_ = PlayStage::kNone;
                Message("已停止");
                break;
            case MusicActionKind::kSeek:
                if (playback_.valid() && WithPlayback([&] { return playback_.Seek(action.value); }))
                    app_.log().Info(("NETEASE_GUEST seek: " + Decimal(action.value)).c_str());
                break;
            case MusicActionKind::kEditQuery:
                state_.editing = true;
                state_.query.clear();
                (void)app_.input().EnableText(true);
                Message("输入歌名或歌手，按回车搜索");
                break;
            case MusicActionKind::kSubmit:
                if (!state_.query.empty()) Browse(MusicContent::kSearch);
                break;
        }
    }
    void LoadMore() {
        if (state_.loading || read_pending_ || !state_.has_more) return;
        if (ImageOperation(client_.operation())) {
            avatar_pending_ |= client_.operation() == Operation::kAvatar;
            cover_pending_ |= client_.operation() == Operation::kCover;
            client_.Cancel();
        }
        ++state_.batch;
        state_.appending = true;
        state_.loading = true;
        read_pending_ = true;
        state_.load_hint = state_.content == MusicContent::kRecommendations ? "正在加载30首新推荐…" : "正在加载更多…";
        app_.log().Info("NETEASE_FEED loading more");
        Pump();
    }
    void FeedComplete(bool failed) {
        const bool appending = state_.appending;
        state_.loading = false;
        state_.appending = false;
        if (!appending) state_.revision = ++revision_;
        if (failed) {
            if (appending) {
                if (state_.batch) --state_.batch;
                state_.has_more = true;
                state_.load_hint = "加载失败，上滑重试";
            }
            return;
        }
        const auto count =
            state_.content == MusicContent::kPlaylists ? client_.playlists.size() : client_.tracks.size();
        state_.has_more = count < Client::kMaxFeedItems && client_.added_items > 0 &&
                          (state_.content == MusicContent::kRecommendations ? true
                           : state_.content == MusicContent::kPlaylists ? client_.more_playlists && state_.batch < 1000
                                                                        : (state_.batch + 1) * 20 < client_.total);
        if (appending && queue_revision_ == state_.revision && state_.content != MusicContent::kPlaylists) {
            queue_ = client_.tracks;
            app_.log().Info(("NETEASE_FEED queue extended: " + Decimal(queue_.size())).c_str());
        }
        state_.load_hint =
            count >= Client::kMaxFeedItems ? "已加载600项，刷新可重新开始"
            : state_.content == MusicContent::kRecommendations && !client_.added_items ? "暂无新推荐，稍后点击刷新"
            : state_.content == MusicContent::kRecommendations && !appending && client_.recommendation_retained
                ? "更新 " + Decimal(client_.added_items) + " 首 · 共 " + Decimal(count) + " 首 · 上滑加载更多"
            : state_.has_more
                ? "已加载 " + Decimal(count) +
                      (state_.content == MusicContent::kPlaylists ? " 个歌单 · 上滑加载更多" : " 首 · 上滑加载更多")
                : "已加载 " + Decimal(count) +
                      (state_.content == MusicContent::kPlaylists ? " 个歌单 · 已全部加载" : " 首 · 已全部加载");
        app_.log().Info(
            ("NETEASE_FEED appended: added=" + Decimal(client_.added_items) + " total=" + Decimal(count)).c_str());
    }
    void Complete(Operation operation) {
        if (operation == Operation::kNone) return;
        const bool failed = !client_.error.empty();
        const auto error = client_.error;
        if (!account_.verified() && owner_) {
            ClearPrivate();
            Message(error.empty() ? "登录已失效，请重新扫码" : error, true);
            return;
        }
        if (operation == Operation::kSetLike) {
            state_.favorite_pending = 0;
            favorite_issued_ = false;
            if (failed) {
                sync_likes_ = true;
                Message("收藏状态未确认，正在重新同步", true);
            } else {
                app_.log().Info(favorite_value_ ? "NETEASE_LIKE confirmed: liked=1"
                                                : "NETEASE_LIKE confirmed: liked=0");
                Message(favorite_value_ ? "已加入我喜欢的音乐" : "已取消喜欢");
            }
            return;
        }
        if (operation == Operation::kLikes) {
            if (failed) {
                state_.favorite_pending = 0;
                Message("喜欢状态暂不可用，点刷新重试", true);
            } else
                app_.log().Info(("NETEASE_LIKES loaded: " + Decimal(client_.liked_ids.size())).c_str());
            return;
        }
        if (ImageOperation(operation)) {
            if (!failed && operation == Operation::kAvatar && account_.verified()) {
                avatar_image_.assign(client_.avatar_bytes().begin(), client_.avatar_bytes().end());
                ++avatar_revision_;
            } else if (!failed && operation == Operation::kCover && cover_id_ == current_.id) {
                cover_image_.assign(client_.avatar_bytes().begin(), client_.avatar_bytes().end());
                ++cover_revision_;
            }
            return;
        }
        if (BrowseOperation(operation)) FeedComplete(failed);
        if (failed) {
            Message(error, true);
            return;
        }
        if (operation == Operation::kSearch) {
            app_.log().Info(("NETEASE_GUEST search: " + Decimal(client_.tracks.size()) + " tracks").c_str());
            Message("搜索完成 · 点击歌曲播放");
        } else if (operation == Operation::kPlaylists) {
            app_.log().Info(("NETEASE_HOME playlists: " + Decimal(client_.playlists.size())).c_str());
            Message(client_.playlists.empty() ? "暂无歌单，创建或收藏后刷新" : "已同步我的歌单");
        } else if (operation == Operation::kRecommendations || operation == Operation::kPlaylistTracks) {
            app_.log().Info(((operation == Operation::kRecommendations ? "NETEASE_HOME recommendations: "
                                                                       : "NETEASE_HOME tracks: ") +
                             Decimal(client_.tracks.size()))
                                .c_str());
            Message(client_.tracks.empty() ? "还没有音乐" : "点击歌曲播放，点击红星收藏");
        }
    }
    void TickPage() {
        bool changed = false;
        if (login_.Poll()) {
            changed = true;
            if (login_.phase == LoginPhase::kAuthenticated)
                Authenticated();
            else if (login_.phase == LoginPhase::kWaiting)
                app_.log().Info("NETEASE_LOGIN waiting");
            else if (login_.phase == LoginPhase::kScanned)
                app_.log().Info("NETEASE_LOGIN scanned");
            else if (login_.phase == LoginPhase::kExpired)
                app_.log().Info("NETEASE_LOGIN expired");
            else if (login_.phase == LoginPhase::kCancelled)
                app_.log().Info("NETEASE_LOGIN cancelled");
            else if (login_.phase == LoginPhase::kError) {
                app_.log().Info(("NETEASE_LOGIN error: " + login_.message).c_str());
                if (!login_visible_ && !initial_started_) StartInitial();
            }
        }
        const auto completed = client_.Poll();
        if (completed != Operation::kNone) {
            Complete(completed);
            changed = true;
        }
        Pump();
        if (changed) Draw();
    }
    void TickPlayback() {
        bool changed = false;
        const auto operation = play_client_.Poll();
        if (operation != Operation::kNone) {
            CompletePlayback(operation);
            changed = true;
        }
        PumpPlayback();
        if (playback_.valid()) {
            const auto media = playback_.State();
            if (media) {
                changed |= media_.position_ms != media->position_ms || media_.phase != media->phase ||
                           media_.duration_ms != media->duration_ms;
                media_ = *media;
                const auto phase = uint16_t(media_.phase);
                if (last_phase_ != phase) {
                    last_phase_ = phase;
                    Message(media_.phase == MediaPhase::kPlaying    ? (trial_ ? "正在试听" : "正在播放")
                            : media_.phase == MediaPhase::kPaused   ? "已暂停"
                            : media_.phase == MediaPhase::kFinished ? "播放结束"
                            : media_.phase == MediaPhase::kFailed   ? media_.message
                                                                    : "正在下载并解码…",
                            media_.phase == MediaPhase::kFailed);
                    if (media_.phase == MediaPhase::kFinished) Step(1);
                    changed = true;
                }
            }
        }
        if (changed) Draw();
    }
    void CompletePlayback(Operation operation) {
        if (!account_.verified() && owner_) {
            ClearPrivate();
            Message("登录已失效，请重新扫码", true);
            return;
        }
        if (!play_client_.error.empty()) {
            Message(play_client_.error, true);
            return;
        }
        if (operation == Operation::kDetail) {
            current_ = play_client_.detail;
            if (queue_.empty()) {
                queue_ = {current_};
                selected_ = 0;
            }
            cover_pending_ = !current_.cover_url.empty();
            play_stage_ = PlayStage::kResolve;
        } else if (operation == Operation::kResolve) {
            trial_ = play_client_.trial;
            app_.log().Info(
                ("NETEASE_GUEST selected: " + Decimal(current_.id) + " trial=" + (trial_ ? "1" : "0")).c_str());
            auto opened = app_.audio().OpenUrl(play_client_.url);
            if (opened) {
                playback_ = std::move(*opened);
                Message(trial_ ? "仅可试听" : "正在下载音频…");
            } else
                Message("暂时无法播放这首歌", true);
        }
    }
    void Tick() {
        TickPlayback();
        if (activity_token_) {
            ComponentContext context(app_, activity_token_);
            context.WithScope([&] { TickPage(); });
        }
        PublishWidget(false);
    }
    void PublishWidget(bool force) {
        const auto now = app_.clock().Now().microseconds();
        const bool playing = play_stage_ != PlayStage::kNone || play_client_.busy() ||
                             (playback_.valid() && media_.phase != MediaPhase::kPaused &&
                              media_.phase != MediaPhase::kFailed && media_.phase != MediaPhase::kFinished);
        const bool transition = widget_playing_ != playing || widget_track_ != current_.id;
        if (!force && !transition && (widget_seen_ == dirty_ || now - widget_at_ < 1000000)) return;
        const auto result = app_.widgets().Publish(
            1,
            {.kind = WidgetTemplate::kMedia,
             .title = Clip(current_.title.empty() ? "网易云音乐" : current_.title, 63),
             .subtitle = Clip(current_.artists.empty() ? (account_.verified() ? account_.nickname : "打开应用选择音乐")
                                                       : current_.artists,
                              95),
             .valid_for_seconds = 0,
             .position_ms = media_.position_ms,
             .duration_ms = media_.duration_ms,
             .value = static_cast<int32_t>(current_.id),
             .auxiliary = static_cast<int32_t>(current_.id >> 32),
             .playing = playing,
             .failed = state_.failed || media_.phase == MediaPhase::kFailed});
        if (result) {
            widget_seen_ = dirty_;
            widget_at_ = now;
            widget_playing_ = playing;
            widget_track_ = current_.id;
        }
    }
    int32_t Command(const AppMessage& message) {
        if (message.type == static_cast<uint16_t>(AppAction::kNextTrack))
            Step(1);
        else if (message.type == static_cast<uint16_t>(AppAction::kSetPlaying)) {
            const auto value = message.U32();
            if (!value || *value > 1) return -1;
            if (*value) {
                if (playback_.valid() && media_.phase != MediaPhase::kFinished && media_.phase != MediaPhase::kFailed)
                    (void)playback_.Pause(false);
                else if (current_.id && !play_client_.busy() && play_stage_ == PlayStage::kNone)
                    StartTrack(current_, false);
            } else {
                if (playback_.valid())
                    (void)playback_.Pause(true);
                else {
                    play_client_.Cancel();
                    play_stage_ = PlayStage::kNone;
                }
            }
            app_.log().Info(*value ? "NETEASE_WIDGET playing=1" : "NETEASE_WIDGET playing=0");
        } else
            return -1;
        if (playback_.valid()) {
            auto state = playback_.State();
            if (state) media_ = *state;
        }
        Draw();
        PublishWidget(true);
        return 0;
    }
    friend class MusicActivity;
    friend class PlaybackService;
    void PumpPlayback() {
        if (play_stage_ != PlayStage::kNone) {
            const bool started = play_stage_ == PlayStage::kDetail ? play_client_.Detail(current_.id)
                                                                   : play_client_.Resolve(current_.id);
            play_stage_ = PlayStage::kNone;
            if (!started) Message(play_client_.error, true);
            return;
        }
    }
    enum class PlayStage { kNone, kDetail, kResolve };
    Application& app_;
    Account account_;
    Client client_;
    Client play_client_;
    Login login_;
    uint32_t service_token_{}, activity_token_{}, dirty_{1}, avatar_revision_{}, cover_revision_{};
    std::vector<uint8_t> avatar_image_, cover_image_;
    bool initialized_{}, destroy_test_done_{};
    uint32_t widget_seen_{};
    uint64_t widget_at_{};
    uint64_t widget_track_{};
    bool widget_playing_{};
    MusicState state_;
    MusicScreen return_screen_{MusicScreen::kSearch};
    MediaPlayback playback_;
    MediaState media_{};
    Track current_;
    std::vector<Track> queue_;
    int selected_{-1};
    uint64_t initial_song_{}, playlist_id_{}, owner_{}, cover_id_{};
    uint32_t revision_{}, queue_revision_{UINT32_MAX};
    uint16_t last_phase_{};
    PlayStage play_stage_{PlayStage::kNone};
    bool explicit_query_{}, initial_started_{}, login_visible_{}, read_pending_{}, sync_likes_{}, reset_playlist_{},
        favorite_value_{}, favorite_issued_{}, avatar_pending_{}, cover_pending_{}, trial_{};
};
}  // namespace netease
