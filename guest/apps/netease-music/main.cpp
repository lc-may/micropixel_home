// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "login_view.hpp"
#include "music_view.hpp"
using namespace micropixel;
using namespace micropixel::literals;
namespace netease {
class MusicApplication final {
   public:
    MusicApplication()
        : scene_(CreateScene(app_)),
          account_(app_),
          client_(app_, account_),
          login_(app_, account_),
          view_(scene_),
          login_view_(scene_),
          timer_(app_.timers().Every(100_ms).value()) {}
    int Run() {
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
        if (app_.launch_arguments().HasFlag("--login"))
            OpenAccount();
        else if (account_.HasToken())
            login_.Verify();
        else
            StartInitial();
        Draw();
        app_.log().Info("NETEASE_GUEST started: SDK Scene + Network HTTP + Audio URL");
        app_.log().Info("NETEASE_GUEST version: 0.4.2");
        app_.Run([&](const Event& event) { return Handle(event); });
        return 0;
    }

   private:
    static Scene CreateScene(Application& app) {
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        return std::move(app.renderer().CreateScene(Color::Rgb(247, 248, 250)).value());
    }
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
    void Draw() {
        view_.Update(account_, client_, state_, current_, media_, playback_.valid());
        if (login_visible_) login_view_.Update(login_, account_);
        app_.renderer().Present(scene_).value();
    }
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
        login_view_.Show(true);
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
        login_view_.Show(false);
        if (!initial_started_) StartInitial();
    }
    void ClearPrivate() {
        client_.ClearLibrary();
        playback_.Close();
        queue_.clear();
        queue_revision_ = UINT32_MAX;
        current_ = {};
        media_ = {};
        view_.ClearImages();
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
        view_.ClearImages();
        owner_ = account_.user_id;
        avatar_pending_ = true;
        sync_likes_ = true;
        login_visible_ = false;
        login_view_.Show(false);
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
        if (play_stage_ != PlayStage::kNone) {
            const bool started =
                play_stage_ == PlayStage::kDetail ? client_.Detail(current_.id) : client_.Resolve(current_.id);
            play_stage_ = PlayStage::kNone;
            if (!started) Message(client_.error, true);
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
        if (!client_.mutation_busy()) {
            if (BrowseOperation(client_.operation())) read_pending_ = true;
            client_.Cancel();
        }
        if (track.id != current_.id) view_.ClearCover();
        current_ = std::move(track);
        playback_.Close();
        media_ = {};
        last_phase_ = 0;
        cover_pending_ = !current_.cover_url.empty();
        play_stage_ = current_.cover_url.empty() ? PlayStage::kDetail : PlayStage::kResolve;
        if (open && state_.screen != MusicScreen::kPlayer) {
            return_screen_ = state_.screen;
            state_.screen = MusicScreen::kPlayer;
        }
        Message("正在获取播放地址…");
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
                    (void)playback_.Pause(media_.phase == MediaPhase::kPlaying);
                else if (!playback_.valid() && current_.id)
                    StartTrack(current_, state_.screen == MusicScreen::kPlayer);
                break;
            case MusicActionKind::kStop:
                if (client_.operation() == Operation::kResolve || client_.operation() == Operation::kDetail ||
                    client_.operation() == Operation::kCover)
                    client_.Cancel();
                cover_pending_ = false;
                playback_.Close();
                media_ = {};
                play_stage_ = PlayStage::kNone;
                Message("已停止");
                break;
            case MusicActionKind::kSeek:
                if (playback_.valid() && playback_.Seek(action.value))
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
            count >= Client::kMaxFeedItems                                             ? "已加载600项，刷新可重新开始"
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
            if (!failed && operation == Operation::kAvatar && account_.verified() &&
                view_.SetAvatar(app_, client_.avatar_bytes()))
                app_.log().Info("NETEASE_HOME avatar ready");
            else if (!failed && operation == Operation::kCover && cover_id_ == current_.id &&
                     view_.SetCover(app_, client_.avatar_bytes()))
                app_.log().Info("NETEASE_PLAYER cover ready");
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
        } else if (operation == Operation::kDetail) {
            current_ = client_.detail;
            if (queue_.empty()) {
                queue_ = {current_};
                selected_ = 0;
            }
            cover_pending_ = !current_.cover_url.empty();
            play_stage_ = PlayStage::kResolve;
        } else if (operation == Operation::kResolve) {
            trial_ = client_.trial;
            app_.log().Info(
                ("NETEASE_GUEST selected: " + Decimal(current_.id) + " trial=" + (trial_ ? "1" : "0")).c_str());
            auto opened = app_.audio().OpenUrl(client_.url);
            if (opened) {
                playback_ = std::move(*opened);
                Message(trial_ ? "仅可试听" : "正在下载音频…");
            } else
                Message("暂时无法播放这首歌", true);
        }
    }
    EventResult Handle(const Event& event) {
        if (event.type() == EventType::kStop) {
            login_.Cancel();
            client_.Cancel();
            playback_.Close();
            StopEditing();
            return EventResult::kExit;
        }
        bool changed = false;
        if (const auto* touch = event.touch()) {
            if (login_visible_) {
                const auto action = login_view_.Touch(*touch, changed);
                if (action == LoginAction::kBack)
                    CloseAccount();
                else if (action == LoginAction::kRefresh) {
                    const bool remember = account_.remember();
                    ClearPrivate();
                    account_.Clear();
                    (void)account_.SetRemember(remember);
                    login_.Start();
                } else if (action == LoginAction::kRemember) {
                    if (!account_.SetRemember(!account_.remember())) login_.message = "无法保存登录状态";
                } else if (action == LoginAction::kLogout) {
                    ClearPrivate();
                    login_.Logout();
                    Message("已退出登录");
                }
                if (action != LoginAction::kNone) changed = true;
            } else {
                const auto action = view_.Touch(*touch, client_, current_, changed);
                if (action.kind != MusicActionKind::kNone) {
                    Act(action);
                    changed = true;
                }
            }
        }
        if (const auto* key = event.key();
            key && (key->phase() == KeyPhase::kDown || key->phase() == KeyPhase::kRepeat)) {
            if (key->code() == KeyCode::kBack && login_visible_) {
                CloseAccount();
                changed = true;
            } else if (!login_visible_ && key->code() == KeyCode::kConfirm) {
                Act({state_.editing ? MusicActionKind::kSubmit : MusicActionKind::kToggle});
                changed = true;
            } else if (state_.editing && key->code() == KeyCode::kBack && !state_.query.empty()) {
                auto n = state_.query.size() - 1;
                while (n && (uint8_t(state_.query[n]) & 0xc0) == 0x80) --n;
                state_.query.resize(n);
                changed = true;
            }
        }
        if (event.TimerFrom(timer_)) {
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
            if (state_.editing) {
                auto text = app_.input().ReadText();
                if (text && !text->empty() && state_.query.size() + text->size() <= 128) {
                    state_.query += text->c_str();
                    changed = true;
                }
            }
            const auto completed = client_.Poll();
            if (completed != Operation::kNone) {
                Complete(completed);
                changed = true;
            }
            Pump();
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
        }
        if (changed) Draw();
        return EventResult::kContinue;
    }
    enum class PlayStage { kNone, kDetail, kResolve };
    Application app_;
    Scene scene_;
    Account account_;
    Client client_;
    Login login_;
    MusicView view_;
    LoginView login_view_;
    Timer timer_;
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
int main() {
    auto application = std::make_unique<netease::MusicApplication>();
    return application->Run();
}
