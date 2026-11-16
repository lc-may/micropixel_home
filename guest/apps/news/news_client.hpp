// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "sdk/json.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/wrap_text.hpp"

namespace news {
using namespace micropixel;
enum class Source : uint32_t { Toutiao = 0, Cctv = 1 };
inline constexpr const char* CctvFeed = "https://news.cctv.com/2019/07/gaiban/cmsdatainterface/page/news_1.jsonp";
inline const char* SourceName(Source source) { return source == Source::Cctv ? "央视网" : "今日头条"; }
inline constexpr unsigned MaxArticles = 50, CachedArticles = 12, MaxBody = 262144;
struct Article {
    char title[256]{};
    uint64_t heat{}, key{};
    char path[64]{};
};
inline bool ValidTitle(std::string_view title) {
    return !title.empty() && title.size() < sizeof(Article::title) && ui::ValidUtf8(title) &&
           std::any_of(
               title.begin(), title.end(), [](uint8_t c) { return c > 32; }) &&
           std::none_of(title.begin(), title.end(), [](uint8_t c) { return c < 32 || c == 127; });
}
inline uint64_t TitleKey(std::string_view title) {
    uint64_t key = 14695981039346656037ULL;
    for (uint8_t c : title) key = (key ^ c) * 1099511628211ULL;
    return key ? key : 1;
}
inline FixedString<48> HeatText(uint64_t heat) {
    FixedString<48> text;
    if (heat >= 100000000) {
        text.AppendFixed(float(heat) / 100000000, 1);
        text.Append("亿");
    } else if (heat >= 10000) {
        text.AppendFixed(float(heat) / 10000, 1);
        text.Append("万");
    } else
        text.AppendUint(heat);
    return text;
}
class NewsClient final {
   public:
    bool Start(const Application& app, Source source = Source::Toutiao) {
        source_ = source;
        Cancel();
        const char* override_url =
            app.launch_arguments().FindValue(source == Source::Cctv ? "--news-content-origin" : "--news-origin");
        auto opened = app.network().Exchange(
            HttpMethod::kGet,
            override_url             ? override_url
            : source == Source::Cctv ? CctvFeed
                                     : "https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc",
            "User-Agent: Mozilla/5.0 MicroPixel/1.0\r\nAccept: application/json\r\nReferer: "
            "https://www.toutiao.com/\r\n",
            {}, MaxBody, {.receive_timeout_ms = 15000});
        if (!opened) return false;
        request_.emplace(std::move(*opened));
        return true;
    }
    int Poll(std::vector<Article>& articles) {
        if (!request_) return -1;
        auto state = request_->State();
        if (!state) return -1;
        if (state->phase == HttpPhase::kPending) return 0;
        auto metadata = request_->Metadata();
        if (state->phase != HttpPhase::kComplete || !metadata || metadata->status_code != 200 || !state->body_length ||
            state->body_length > MaxBody)
            return -1;
        std::string body(state->body_length, '\0');
        for (uint32_t at = 0; at < body.size();) {
            auto bytes = std::min<uint32_t>(4096, body.size() - at);
            auto got = request_->Read(at, {reinterpret_cast<uint8_t*>(body.data()) + at, bytes});
            if (!got || *got != bytes) return -1;
            at += bytes;
        }
        return (source_ == Source::Cctv ? ParseCctv(body, articles) : Parse(body, articles)) ? 1 : -1;
    }
    void Cancel() { request_.reset(); }
    static bool Parse(std::string_view body, std::vector<Article>& output) {
        Json json(body, 16384);
        if (!json.valid()) return false;
        auto data = json.Field(0, "data");
        if (json.Type(data) != Json::kArray) return false;
        auto status = json.Field(0, "status");
        if (status >= 0 && !((json.Type(status) == Json::kString && json.String(status, 32) == "success") ||
                             (json.Type(status) == Json::kNumber && json.Real(status) && *json.Real(status) == 0)))
            return false;
        std::vector<Article> parsed;
        parsed.reserve(MaxArticles);
        for (int item = json.First(data); item >= 0 && parsed.size() < MaxArticles; item = json.Next(data, item)) {
            auto title = json.String(json.Field(item, "Title"), 255);
            for (auto& c : title)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (!ValidTitle(title)) continue;
            auto token = json.Field(item, "HotValue");
            std::string value;
            if (json.Type(token) == Json::kString)
                value = json.String(token, 16);
            else if (auto number = json.Real(token);
                     number && *number >= 0 && *number <= 1000000000000ULL && uint64_t(*number) == *number)
                value = std::to_string(uint64_t(*number));
            if (value.empty()) continue;
            uint64_t heat = 0;
            bool valid = true;
            for (char c : value) {
                if (c < '0' || c > '9' || heat > (1000000000000ULL - unsigned(c - '0')) / 10) {
                    valid = false;
                    break;
                }
                heat = heat * 10 + unsigned(c - '0');
            }
            if (!valid) continue;
            uint64_t key = TitleKey(title);
            if (std::any_of(parsed.begin(), parsed.end(), [key](const auto& a) { return a.key == key; })) continue;
            Article article;
            std::memcpy(article.title, title.c_str(), title.size() + 1);
            article.heat = heat;
            article.key = key;
            auto id = json.String(json.Field(item, "ClusterIdStr"), 23);
            if (!id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                auto path = "trending/" + id + "/";
                std::memcpy(article.path, path.c_str(), path.size() + 1);
            }
            parsed.push_back(article);
        }
        if (json.Count(data) && parsed.empty()) return false;
        output = std::move(parsed);
        return true;
    }

    static bool ValidCctvPath(std::string_view path) {
        if (path.size() < 25 || path.size() >= 64 || !path.ends_with(".shtml") || path[4] != '/' || path[7] != '/' ||
            path[10] != '/' || !path.substr(11).starts_with("ARTI"))
            return false;
        for (unsigned i = 0; i < 10; ++i)
            if (i != 4 && i != 7 && (path[i] < '0' || path[i] > '9')) return false;
        for (char c : path.substr(11, path.size() - 17))
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        return true;
    }
    static bool ParseCctv(std::string_view body, std::vector<Article>& output) {
        while (!body.empty() && (body.front() == ' ' || body.front() == '\r' || body.front() == '\n'))
            body.remove_prefix(1);
        if (!body.starts_with('{')) {
            auto open = body.find('('), close = body.rfind(')');
            if (open == body.npos || close == body.npos || open > 96 || close <= open) return false;
            for (char c : body.substr(0, open))
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$'))
                    return false;
            auto tail = body.substr(close + 1);
            if (tail.find_first_not_of("; \r\n\t") != tail.npos) return false;
            body = body.substr(open + 1, close - open - 1);
        }
        Json json(body, 16384);
        if (!json.valid()) return false;
        auto list = json.Field(json.Field(0, "data"), "list");
        if (json.Type(list) != Json::kArray) return false;
        std::vector<Article> parsed;
        parsed.reserve(MaxArticles);
        for (int item = json.First(list); item >= 0 && parsed.size() < MaxArticles; item = json.Next(list, item)) {
            auto title = json.String(json.Field(item, "title"), 255);
            auto url = json.String(json.Field(item, "url"), 256);
            std::string_view path(url);
            if (path.starts_with("https://news.cctv.com/"))
                path.remove_prefix(22);
            else if (path.starts_with("http://news.cctv.com/"))
                path.remove_prefix(21);
            else
                continue;
            if (!ValidTitle(title) || !ValidCctvPath(path)) continue;
            auto key = TitleKey(path);
            if (std::any_of(parsed.begin(), parsed.end(), [key](const auto& a) { return a.key == key; })) continue;
            Article article;
            std::memcpy(article.title, title.c_str(), title.size() + 1);
            std::memcpy(article.path, path.data(), path.size());
            article.key = key;
            parsed.push_back(article);
        }
        if (json.Count(list) && parsed.empty()) return false;
        output = std::move(parsed);
        return true;
    }

   private:
    Source source_{};
    std::optional<HttpRequest> request_;
};
struct Model {
    struct Saved {
        uint32_t version{2}, count{};
        uint64_t updated{};
        Source source{Source::Cctv};
        std::array<Article, CachedArticles> articles{};
    };
    struct Reads {
        uint32_t version{1}, count{};
        std::array<uint64_t, MaxArticles> keys{};
    } reads;
    std::vector<Article> articles;
    Source source{Source::Cctv};
    uint64_t updated{};
    bool cached{}, failed{}, busy{};
    const char* CacheKey() const { return source == Source::Cctv ? "news.cctv" : "news.toutiao"; }
    void Load(const Application& app) {
        auto stored = app.storage().GetU32Or("news.source", 1);
        source = stored == 0 ? Source::Toutiao : Source::Cctv;
        if (const char* option = app.launch_arguments().FindValue("--news-source"))
            source = std::string_view(option) == "toutiao" ? Source::Toutiao : Source::Cctv;
        auto read = app.storage().GetBytes("news.read", reinterpret_cast<uint8_t*>(&reads), sizeof(reads));
        if (!read || *read != sizeof(reads) || reads.version != 1 || reads.count > MaxArticles) reads = {};
        LoadFeed(app);
    }
    void LoadFeed(const Application& app) {
        articles.clear();
        updated = 0;
        cached = failed = busy = false;
        auto saved = std::make_unique<Saved>();
        auto got = app.storage().GetBytes(CacheKey(), reinterpret_cast<uint8_t*>(saved.get()), sizeof(Saved));
        if (got && *got == sizeof(Saved) && saved->version == 2 && saved->source == source &&
            saved->count <= CachedArticles) {
            bool valid = true;
            for (unsigned i = 0; i < saved->count; ++i) {
                auto& a = saved->articles[i];
                valid &=
                    a.title[255] == 0 && a.path[63] == 0 && ValidTitle(a.title) &&
                    a.key == TitleKey(source == Source::Cctv ? std::string_view(a.path) : std::string_view(a.title)) &&
                    (source != Source::Cctv || NewsClient::ValidCctvPath(a.path)) && a.heat <= 1000000000000ULL;
            }
            if (valid) {
                articles.assign(saved->articles.begin(), saved->articles.begin() + saved->count);
                updated = saved->updated;
                cached = true;
            }
        }
        // Import the previous title-only Toutiao cache without losing read state.
        if (!cached && source == Source::Toutiao) {
            struct LegacyArticle {
                char title[256]{};
                uint64_t heat{}, key{};
            };
            struct LegacySaved {
                uint32_t version{}, count{};
                uint64_t updated{};
                std::array<LegacyArticle, CachedArticles> articles{};
            };
            auto old = std::make_unique<LegacySaved>();
            auto n = app.storage().GetBytes("news.cache", reinterpret_cast<uint8_t*>(old.get()), sizeof(LegacySaved));
            if (n && *n == sizeof(LegacySaved) && old->version == 1 && old->count <= CachedArticles) {
                bool valid = true;
                for (unsigned i = 0; i < old->count; ++i) {
                    const auto& a = old->articles[i];
                    if (a.title[255] || !ValidTitle(a.title) || a.key != TitleKey(a.title) ||
                        a.heat > 1000000000000ULL) {
                        valid = false;
                        break;
                    }
                    Article row;
                    std::memcpy(row.title, a.title, sizeof(row.title));
                    row.heat = a.heat;
                    row.key = a.key;
                    articles.push_back(row);
                }
                if (valid) {
                    updated = old->updated;
                    cached = true;
                } else
                    articles.clear();
            }
        }
        if (cached) {
            FixedString<64> log;
            log.Append("NEWS cache loaded: ");
            log.AppendUint(articles.size());
            app.log().Info(log.c_str());
        }
    }
    void Save(const Application& app) {
        auto saved = std::make_unique<Saved>();
        saved->source = source;
        saved->count = std::min<size_t>(CachedArticles, articles.size());
        saved->updated = updated;
        std::copy_n(articles.begin(), saved->count, saved->articles.begin());
        (void)app.storage().SetBytes(CacheKey(), reinterpret_cast<const uint8_t*>(saved.get()), sizeof(Saved));
    }
    void Choose(const Application& app, Source selected) {
        source = selected;
        (void)app.storage().SetU32("news.source", static_cast<uint32_t>(source));
        LoadFeed(app);
    }
    bool Read(uint64_t key) const {
        return std::find(reads.keys.begin(), reads.keys.begin() + reads.count, key) != reads.keys.begin() + reads.count;
    }
    void MarkRead(const Application& app, uint64_t key) {
        if (Read(key)) return;
        if (reads.count == MaxArticles) {
            std::move(reads.keys.begin() + 1, reads.keys.end(), reads.keys.begin());
            --reads.count;
        }
        reads.keys[reads.count++] = key;
        (void)app.storage().SetBytes("news.read", reinterpret_cast<const uint8_t*>(&reads), sizeof(reads));
    }
};
static_assert(sizeof(Model::Saved) <= 4096);
}  // namespace news
