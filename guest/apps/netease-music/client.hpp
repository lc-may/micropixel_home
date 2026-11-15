// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "account.hpp"
#include "json.hpp"
#include "sdk/application.hpp"
#include "weapi.hpp"
namespace netease {
struct Track {
    uint64_t id{};
    std::string title, artists, cover_url;
    uint32_t duration{};
};
struct Playlist {
    uint64_t id{};
    std::string name;
    uint32_t track_count{};
    bool created{};
};
enum class Operation {
    kNone,
    kSearch,
    kDetail,
    kResolve,
    kPlaylists,
    kRecommendations,
    kPlaylistIndex,
    kPlaylistTracks,
    kAvatar,
    kCover,
    kLikes,
    kSetLike
};
class Client {
   public:
    static constexpr size_t kMaxFeedItems = 600;
    explicit Client(micropixel::Application& app, Account& account) : app_(app), account_(account) {
        if (const char* origin = app.launch_arguments().FindValue("--test-origin")) {
            std::string_view value = origin;
            if (value.starts_with("http://127.0.0.1:") && value.find_first_of("/@?#", 7) == std::string_view::npos) {
                base_ = value;
                fixture_ = true;
            }
        }
    }
    bool Search(std::string_view query, uint32_t page, bool append = false) {
        append_ = append;
        added_items = received_items = 0;
        return Start(Operation::kSearch, "/weapi/search/get",
                     "\"s\":" + Quote(query) + ",\"type\":1,\"limit\":20,\"offset\":" + Decimal(page * 20));
    }
    bool Detail(uint64_t id) {
        expected_id_ = id;
        return Start(Operation::kDetail, "/weapi/v3/song/detail", "\"c\":" + Quote("[{\"id\":" + Decimal(id) + "}]"));
    }
    bool Resolve(uint64_t id) {
        expected_id_ = id;
        return Start(Operation::kResolve, "/weapi/song/enhance/player/url",
                     "\"ids\":" + Quote("[" + Decimal(id) + "]") + ",\"br\":128000");
    }
    bool Playlists(uint32_t page = 0, bool append = false) {
        if (!Personal()) return false;
        if (page > 1000) {
            error = "已达到歌单分页上限";
            return false;
        }
        append_ = append;
        added_items = received_items = 0;
        more_playlists = false;
        playlist_truncated = false;
        return Start(Operation::kPlaylists, "/weapi/user/playlist",
                     "\"uid\":" + Decimal(account_.user_id) + ",\"limit\":20,\"offset\":" + Decimal(page * 20) +
                         ",\"includeVideo\":false");
    }
    bool Recommendations(bool append = false) {
        if (!Personal()) return false;
        append_ = append;
        added_items = received_items = 0;
        recommendation_attempts_ = 0;
        recommendation_retained = 0;
        recommendation_stage_.clear();
        if (!append && !recommendation_cache_.empty()) tracks = recommendation_cache_;
        total = tracks.size();
        return FreshRecommendations();
    }
    bool PlaylistTracks(uint64_t id, uint32_t page = 0, bool refresh = false, bool append = false) {
        if (!Personal()) return false;
        if (!id || page >= 100) {
            error = "歌单参数超出范围";
            return false;
        }
        playlist_page_ = page;
        append_ = append;
        added_items = received_items = 0;
        if (refresh || playlist_id_ != id) {
            playlist_id_ = id;
            playlist_ids_.clear();
            return Start(Operation::kPlaylistIndex, "/weapi/v6/playlist/detail",
                         "\"id\":" + Decimal(id) + ",\"n\":0,\"s\":0");
        }
        return SongPage();
    }
    bool Avatar() { return Image(account_.avatar_url, Operation::kAvatar, 96); }
    bool Cover(const Track& track) { return Image(track.cover_url, Operation::kCover, 256); }
    bool Likes() {
        if (!Personal()) return false;
        likes_ready = false;
        return Start(Operation::kLikes, "/weapi/song/like/get", "\"uid\":" + Decimal(account_.user_id));
    }
    bool Liked(uint64_t id) const { return std::binary_search(liked_ids.begin(), liked_ids.end(), id); }
    bool SetLike(uint64_t id, bool liked) {
        if (!Personal() || !id) return false;
        if (busy() || !likes_ready) {
            error = "收藏状态正在同步，请稍后";
            return false;
        }
        favorite_id_ = id;
        favorite_value_ = liked;
        return Start(Operation::kSetLike, "/weapi/radio/like",
                     "\"alg\":\"itembased\",\"trackId\":" + Decimal(id) + ",\"like\":" + (liked ? "true" : "false") +
                         ",\"time\":\"3\"");
    }
    bool LikedTracks(uint32_t page = 0, bool append = false) {
        if (!Personal() || !likes_ready || page >= 100) {
            error = "请先同步喜欢的音乐";
            return false;
        }
        playlist_id_ = 0;
        append_ = append;
        added_items = received_items = 0;
        playlist_page_ = page;
        playlist_ids_.assign(liked_ids.begin(), liked_ids.begin() + std::min<size_t>(2000, liked_ids.size()));
        playlist_truncated = liked_ids.size() > 2000;
        total = playlist_ids_.size();
        if (playlist_ids_.empty()) {
            error = "还没有喜欢的音乐";
            return false;
        }
        return SongPage();
    }
    Operation operation() const { return request_.valid() ? operation_ : Operation::kNone; }
    bool mutation_busy() const { return operation() == Operation::kSetLike; }
    bool likes_ready{};
    std::vector<uint64_t> liked_ids;
    bool Image(std::string url, Operation kind, uint32_t size) {
        if (mutation_busy()) return false;
        Cancel();
        if (url.starts_with("http://")) url.replace(0, 4, "https");
        if (!OfficialUrl(url)) return false;
        url += (url.find('?') == url.npos ? "?" : "&") + std::string("param=") + Decimal(size) + "y" + Decimal(size);
        auto request =
            app_.network().Exchange(micropixel::HttpMethod::kGet,
                                    fixture_ ? base_ + (kind == Operation::kAvatar ? "/__avatar" : "/__cover") : url,
                                    "Referer: https://music.163.com/\r\n", {}, 65536);
        if (!request) return false;
        request_ = std::move(*request);
        operation_ = kind;
        return true;
    }
    std::span<const uint8_t> avatar_bytes() const {
        return {reinterpret_cast<const uint8_t*>(response_.data()), response_.size()};
    }
    void ClearLibrary() {
        Cancel();
        playlists.clear();
        tracks.clear();
        playlist_ids_.clear();
        playlist_id_ = 0;
        total = 0;
        more_playlists = false;
        recommendation_cache_.clear();
        recommendation_stage_.clear();
        recommendation_seen_.clear();
        seen_cursor_ = 0;
        recommendation_retained = 0;
        liked_ids.clear();
        likes_ready = false;
        playlist_truncated = false;
        response_.clear();
        url.clear();
        detail = {};
    }
    bool busy() const { return request_.valid(); }
    void ClearBrowse() {
        tracks.clear();
        playlists.clear();
        total = 0;
        more_playlists = false;
    }
    Operation Poll() {
        if (!request_.valid()) return Operation::kNone;
        auto state = request_.State();
        if (state && state->phase == micropixel::HttpPhase::kPending) return Operation::kNone;
        auto kind = operation_;
        auto completed = kind == Operation::kPlaylistIndex ? Operation::kPlaylistTracks : kind;
        if (kind == Operation::kSetLike) likes_ready = false;
        response_.clear();
        error.clear();
        if (!state)
            error = "HTTP 状态查询失败";
        else if (state->phase == micropixel::HttpPhase::kFailed)
            error = state->message;
        else {
            response_.resize(state->body_length);
            uint32_t offset = 0;
            while (offset < response_.size()) {
                auto n = std::min<size_t>(4096, response_.size() - offset);
                auto got = request_.Read(offset, {reinterpret_cast<uint8_t*>(response_.data()) + offset, n});
                if (!got || *got == 0) {
                    error = "HTTP 响应读取失败";
                    break;
                }
                offset += *got;
            }
        }
        if (kind == Operation::kAvatar || kind == Operation::kCover) {
            auto meta = request_.Metadata();
            if (!meta || meta->status_code != 200) error = "头像暂不可用";
        }
        request_.Close();
        if (!error.empty()) return completed;
        if (kind == Operation::kAvatar || kind == Operation::kCover) return completed;
        Json json(response_, kind >= Operation::kPlaylists ? 32768 : 4096);
        if (!json.valid()) {
            error = "网易云返回了无效 JSON";
            return completed;
        }
        auto code = json.Number(json.Field(0, "code"));
        if (code != 200) {
            if (code == 301) {
                account_.Clear();
                error = "登录已失效，请重新扫码";
                return completed;
            }
            auto message = json.String(json.Field(0, "msg"), 512);
            if (message.empty()) message = json.String(json.Field(0, "message"), 512);
            error = "网易云请求失败（" + std::string(code < 0 ? "-" : "") + Decimal(code < 0 ? -code : code) + "）：" +
                    message;
            return completed;
        }
        if (kind == Operation::kLikes) {
            const auto ids = json.Field(0, "ids");
            if (json.Type(ids) != Json::kArray || json.Count(ids) > 20000)
                error = "喜欢的音乐列表超出容量或格式无效";
            else {
                std::vector<uint64_t> loaded;
                const auto count = json.Count(ids);
                loaded.reserve(count);
                for (int item = json.First(ids); item >= 0; item = json.Next(ids, item)) {
                    const auto id = json.Number(item);
                    if (id <= 0) {
                        error = "喜欢的音乐编号无效";
                        break;
                    }
                    loaded.push_back(id);
                }
                if (error.empty()) {
                    std::sort(loaded.begin(), loaded.end());
                    loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
                    liked_ids = std::move(loaded);
                    likes_ready = true;
                }
            }
        } else if (kind == Operation::kSetLike) {
            auto at = std::lower_bound(liked_ids.begin(), liked_ids.end(), favorite_id_);
            if (favorite_value_ && (at == liked_ids.end() || *at != favorite_id_)) {
                if (liked_ids.size() >= 20000) {
                    error = "喜欢的音乐已达到同步容量，请刷新";
                    return completed;
                }
                liked_ids.insert(at, favorite_id_);
            } else if (!favorite_value_ && at != liked_ids.end() && *at == favorite_id_)
                liked_ids.erase(at);
            likes_ready = true;
        } else if (completed == Operation::kSearch) {
            auto result = json.Field(0, "result");
            auto songs = json.Field(result, "songs");
            const auto count = json.Number(json.Field(result, "songCount"));
            total = count > 0 ? std::min<int64_t>(count, 20020) : 0;
            std::vector<Track> incoming;
            if (json.Type(result) != Json::kObject || json.Count(songs) > 20 ||
                (total && json.Type(songs) != Json::kArray))
                error = "搜索结果格式无效";
            else
                for (uint32_t i = 0; i < json.Count(songs); ++i) {
                    auto track = ReadTrack(json, json.At(songs, i));
                    if (track.id && !track.title.empty()) incoming.push_back(std::move(track));
                }
            if (error.empty()) {
                received_items = json.Count(songs);
                added_items = Merge(tracks, std::move(incoming));
            }
        } else if (kind == Operation::kPlaylists) {
            auto list = json.Field(0, "playlist");
            if (json.Type(list) != Json::kArray || json.Count(list) > 20)
                error = "歌单列表格式无效";
            else {
                std::vector<Playlist> incoming;
                for (uint32_t i = 0; i < json.Count(list); ++i) {
                    auto entry = json.At(list, i);
                    auto id = json.Number(json.Field(entry, "id"));
                    auto name = json.String(json.Field(entry, "name"), 256);
                    auto count = json.Number(json.Field(entry, "trackCount"));
                    if (id <= 0 || name.empty() || count < 0 || count > UINT32_MAX) continue;
                    auto creator = json.Number(json.Field(json.Field(entry, "creator"), "userId"));
                    incoming.push_back(
                        {uint64_t(id), std::move(name), uint32_t(count), creator == int64_t(account_.user_id)});
                }
                more_playlists = json.Boolean(json.Field(0, "more"));
                received_items = json.Count(list);
                added_items = Merge(playlists, std::move(incoming));
            }
        } else if (kind == Operation::kPlaylistIndex) {
            auto list = json.Field(0, "playlist");
            auto ids = json.Field(list, "trackIds");
            if (json.Number(json.Field(list, "id")) != int64_t(playlist_id_) || json.Type(ids) != Json::kArray)
                error = "歌单详情格式无效";
            else {
                for (uint32_t i = 0; i < std::min<uint32_t>(json.Count(ids), 2000); ++i) {
                    auto id = json.Number(json.Field(json.At(ids, i), "id"));
                    if (id > 0) playlist_ids_.push_back(id);
                }
                total = playlist_ids_.size();
                playlist_truncated = json.Count(ids) > 2000;
                if (!playlist_ids_.empty()) {
                    if (SongPage()) return Operation::kNone;
                }
            }
        } else if (kind == Operation::kRecommendations || kind == Operation::kPlaylistTracks) {
            auto songs = kind == Operation::kRecommendations ? json.Field(json.Field(0, "data"), "dailySongs")
                                                             : json.Field(0, "songs");
            if (kind == Operation::kRecommendations && songs < 0) songs = json.Field(0, "recommend");
            const uint32_t max_songs = kind == Operation::kRecommendations ? 60 : 20;
            if (json.Type(songs) != Json::kArray || json.Count(songs) > max_songs)
                error = "个人歌曲列表格式无效";
            else {
                std::vector<Track> incoming;
                for (uint32_t i = 0; i < json.Count(songs); ++i) {
                    auto track = ReadTrack(json, json.At(songs, i));
                    if (kind == Operation::kPlaylistTracks) {
                        auto begin =
                            playlist_ids_.begin() + std::min<size_t>(playlist_page_ * 20, playlist_ids_.size());
                        auto end = begin + std::min<size_t>(20, playlist_ids_.end() - begin);
                        if (std::find(begin, end, track.id) == end) continue;
                    }
                    if (track.id && !track.title.empty()) incoming.push_back(std::move(track));
                }
                received_items = json.Count(songs);
                if (kind == Operation::kRecommendations) {
                    const auto before = recommendation_stage_.size();
                    for (auto& track : incoming) {
                        if (recommendation_stage_.size() == 30) break;
                        if (std::find(recommendation_seen_.begin(), recommendation_seen_.end(), track.id) !=
                                recommendation_seen_.end() ||
                            std::any_of(recommendation_stage_.begin(), recommendation_stage_.end(),
                                        [&](const Track& old) { return old.id == track.id; }) ||
                            (append_ && std::any_of(tracks.begin(), tracks.end(),
                                                    [&](const Track& old) { return old.id == track.id; })))
                            continue;
                        recommendation_stage_.push_back(std::move(track));
                    }
                    ++recommendation_attempts_;
                    app_.log().Info(("NETEASE_RECOMMEND response: attempt=" + Decimal(recommendation_attempts_) +
                                     " raw=" + Decimal(json.Count(songs)) + " valid=" + Decimal(incoming.size()) +
                                     " new=" + Decimal(recommendation_stage_.size() - before) +
                                     " staged=" + Decimal(recommendation_stage_.size()))
                                        .c_str());
                    if (recommendation_stage_.size() < 30 && recommendation_stage_.size() > before &&
                        recommendation_attempts_ < 3) {
                        if (FreshRecommendations()) return Operation::kNone;
                        return completed;
                    }
                    PublishRecommendations();
                } else {
                    added_items = Merge(tracks, std::move(incoming));
                    total = playlist_ids_.size();
                }
            }
        } else if (completed == Operation::kDetail) {
            auto song = json.At(json.Field(0, "songs"), 0);
            detail = ReadTrack(json, song);
            if (detail.id != expected_id_) error = "未找到歌曲详情";
        } else {
            auto song = json.At(json.Field(0, "data"), 0);
            if (json.Number(json.Field(song, "id")) != int64_t(expected_id_)) error = "播放记录不匹配";
            url = json.String(json.Field(song, "url"));
            trial = json.Type(json.Field(song, "freeTrialInfo")) == Json::kObject;
            if (error.empty() && url.empty()) error = "不可播放：可能需要登录、会员或受版权/地区限制";
            if (error.empty() && !OfficialUrl(url) && !(fixture_ && url.starts_with(base_ + "/")))
                error = "拒绝非网易云音源地址";
            if (error.empty() && OfficialUrl(url) && url.starts_with("http://")) url.replace(0, 4, "https");
        }
        return completed;
    }
    void Cancel() {
        request_.Close();
        operation_ = Operation::kNone;
    }
    std::vector<Track> tracks;
    std::vector<Playlist> playlists;
    Track detail;
    uint32_t total{};
    uint32_t added_items{}, received_items{};
    uint32_t recommendation_retained{};
    std::string error, url;
    bool trial{}, more_playlists{}, playlist_truncated{};

   private:
    void PublishRecommendations() {
        std::vector<uint64_t> accepted;
        for (const auto& track : recommendation_stage_) accepted.push_back(track.id);
        if (!accepted.empty()) {
            // New songs lead a refreshed page. Keep old recommendations to fill a short update.
            // An append never pads: repeated songs must not reappear at the bottom.
            if (!append_)
                for (const auto& old : recommendation_cache_) {
                    if (recommendation_stage_.size() >= 30) break;
                    if (std::any_of(recommendation_stage_.begin(), recommendation_stage_.end(),
                                    [&](const Track& track) { return track.id == old.id; }))
                        continue;
                    recommendation_stage_.push_back(old);
                }
            added_items = Merge(tracks, std::move(recommendation_stage_));
            if (!append_) {
                recommendation_retained = added_items - accepted.size();
                added_items = accepted.size();
            }
            recommendation_cache_ = tracks;
            for (auto id : accepted) {
                if (recommendation_seen_.size() < 4096)
                    recommendation_seen_.push_back(id);
                else {
                    recommendation_seen_[seen_cursor_] = id;
                    seen_cursor_ = (seen_cursor_ + 1) % 4096;
                }
            }
        } else if (!append_)
            recommendation_retained = tracks.size();
        total = tracks.size();
        app_.log().Info(("NETEASE_RECOMMEND publish: append=" + Decimal(append_) + " new=" + Decimal(added_items) +
                         " retained=" + Decimal(recommendation_retained) + " total=" + Decimal(total))
                            .c_str());
    }
    bool FreshRecommendations() {
        return Start(Operation::kRecommendations, "/weapi/v3/discovery/recommend/songs", "\"afresh\":true");
    }
    template <class T>
    uint32_t Merge(std::vector<T>& destination, std::vector<T> incoming) {
        if (!append_) destination.clear();
        uint32_t added = 0;
        for (auto& item : incoming) {
            if (destination.size() >= kMaxFeedItems) break;
            if (std::any_of(destination.begin(), destination.end(), [&](const T& old) { return old.id == item.id; }))
                continue;
            destination.push_back(std::move(item));
            ++added;
        }
        return added;
    }
    bool Personal() {
        if (account_.verified() && account_.user_id) return true;
        error = "请先登录网易云账号";
        return false;
    }
    bool SongPage() {
        auto offset = playlist_page_ * 20;
        if (offset >= playlist_ids_.size()) {
            error = "歌单没有更多歌曲";
            return false;
        }
        std::string ids = "[";
        for (uint32_t i = offset; i < std::min<size_t>(offset + 20, playlist_ids_.size()); ++i) {
            if (i != offset) ids += ",";
            ids += "{\"id\":" + Decimal(playlist_ids_[i]) + "}";
        }
        return Start(Operation::kPlaylistTracks, "/weapi/v3/song/detail", "\"c\":" + Quote(ids + "]"));
    }
    bool Start(Operation op, std::string_view route, const std::string& data) {
        if (mutation_busy()) {
            error = "正在同步收藏，请稍后";
            return false;
        }
        Cancel();
        error.clear();
        auto body = Weapi("{" + data + ",\"csrf_token\":" + Quote(account_.Csrf()) + ",\"e_r\":false}", app_.random());
        std::string headers =
            "Content-Type: application/x-www-form-urlencoded\r\nReferer: https://music.163.com/\r\nUser-Agent: "
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/124.0.0.0 Safari/537.36\r\nCookie: os=pc; "
            "appver=3.1.17.204416; channel=netease\r\n";
        auto cookies = account_.jar.For("https://music.163.com" + std::string(route));
        if (!cookies.empty()) {
            headers.resize(headers.size() - 2);
            headers += "; " + cookies + "\r\n";
        }
        auto request = app_.network().Start(micropixel::HttpMethod::kPost, base_ + std::string(route), headers,
                                            {reinterpret_cast<const uint8_t*>(body.data()), body.size()});
        if (!request) {
            error = "Host 尚未提供 HTTP 服务";
            return false;
        }
        request_ = std::move(*request);
        operation_ = op;
        return true;
    }
    static Track ReadTrack(const Json& json, int song) {
        Track t;
        auto id = json.Number(json.Field(song, "id"));
        if (id > 0) t.id = id;
        t.title = json.String(json.Field(song, "name"), 512);
        auto album = json.Field(song, "al");
        if (album < 0) album = json.Field(song, "album");
        t.cover_url = json.String(json.Field(album, "picUrl"));
        auto duration = json.Number(json.Field(song, "duration"));
        if (!duration) duration = json.Number(json.Field(song, "dt"));
        t.duration = duration > 0 && duration <= UINT32_MAX ? duration : 0;
        auto artists = json.Field(song, "artists");
        if (artists < 0) artists = json.Field(song, "ar");
        for (uint32_t i = 0; i < std::min<uint32_t>(json.Count(artists), 8); ++i) {
            auto name = json.String(json.Field(json.At(artists, i), "name"), 128);
            if (!t.artists.empty()) t.artists += " / ";
            t.artists += name;
        }
        return t;
    }
    static bool OfficialUrl(std::string_view url) {
        if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
        auto begin = url.find("://") + 3;
        auto end = url.find_first_of("/?#", begin);
        auto host = url.substr(begin, end - begin);
        return host == "music.126.net" || host.ends_with(".music.126.net") || host == "music.163.com" ||
               host.ends_with(".music.163.com");
    }
    micropixel::Application& app_;
    Account& account_;
    micropixel::HttpRequest request_;
    Operation operation_{};
    uint64_t expected_id_{};
    uint64_t favorite_id_{};
    bool favorite_value_{};
    uint64_t playlist_id_{};
    uint32_t playlist_page_{};
    std::vector<uint64_t> playlist_ids_;
    std::vector<Track> recommendation_cache_, recommendation_stage_;
    std::vector<uint64_t> recommendation_seen_;
    uint32_t recommendation_attempts_{}, seen_cursor_{};
    bool append_{};
    std::string response_;
    std::string base_ = "https://music.163.com";
    bool fixture_{};
};
}  // namespace netease
