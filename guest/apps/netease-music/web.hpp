// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "sdk/application.hpp"
namespace netease {
inline std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}
inline std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}
inline bool SafeText(std::string_view text) {
    for (unsigned char c : text)
        if (c < 32 || c == 127) return false;
    return true;
}
inline std::string UrlEncode(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline std::string UrlDecode(std::string_view text) {
    auto hex = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%') {
            if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return {};
            out += char(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else
            out += text[i] == '+' ? ' ' : text[i];
    }
    return SafeText(out) ? out : std::string{};
}
inline std::string Query(std::string_view url, std::string_view name) {
    url = url.substr(0, url.find('#'));
    auto q = url.find('?');
    if (q == url.npos) return {};
    url.remove_prefix(q + 1);
    while (!url.empty()) {
        auto end = url.find('&');
        auto part = url.substr(0, end);
        auto eq = part.find('=');
        if (part.substr(0, eq) == name) return eq == part.npos ? std::string{} : UrlDecode(part.substr(eq + 1));
        if (end == url.npos) break;
        url.remove_prefix(end + 1);
    }
    return {};
}
struct WebUrl {
    std::string_view host, path, origin;
};
inline WebUrl SplitUrl(std::string_view url) {
    if (!url.starts_with("https://")) return {};
    auto end = url.find_first_of("/?#", 8);
    auto origin = url.substr(0, end);
    auto path = end == url.npos ? std::string_view("/") : url.substr(end, url.find_first_of("?#", end) - end);
    if (path.empty()) path = "/";
    return {origin.substr(8), path, origin};
}
inline bool TrustedAuthUrl(std::string_view url) {
    if (url.size() > 2048 || !SafeText(url) || url.find('\\') != url.npos) return false;
    auto parts = SplitUrl(url);
    return parts.host == "music.163.com" || parts.host == "open.weixin.qq.com" ||
           parts.host == "long.open.weixin.qq.com" || parts.host == "lp.open.weixin.qq.com";
}
inline std::string Header(std::string_view headers, std::string_view name) {
    while (!headers.empty()) {
        auto end = headers.find('\n');
        auto line = Trim(headers.substr(0, end));
        auto colon = line.find(':');
        if (colon != line.npos && Lower(line.substr(0, colon)) == name)
            return std::string(Trim(line.substr(colon + 1)));
        if (end == headers.npos) break;
        headers.remove_prefix(end + 1);
    }
    return {};
}
inline bool DomainMatches(std::string_view host, std::string_view domain) {
    return host == domain ||
           (host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.');
}
class CookieJar {
   public:
    void Clear() {
        for (auto& c : cookies_) c = {};
    }
    std::string For(std::string_view url) const {
        auto parts = SplitUrl(url);
        std::string out;
        if (parts.host.empty()) return out;
        for (const auto& c : cookies_) {
            if (c.name.empty() || (c.host_only ? parts.host != c.domain : !DomainMatches(parts.host, c.domain)))
                continue;
            if (!parts.path.starts_with(c.path) ||
                (parts.path.size() > c.path.size() && !c.path.ends_with('/') && parts.path[c.path.size()] != '/'))
                continue;
            if (!out.empty()) out += "; ";
            out += c.name + '=' + c.value;
        }
        return out;
    }
    void Receive(std::string_view url, std::string_view headers) {
        while (!headers.empty()) {
            auto end = headers.find('\n');
            auto line = Trim(headers.substr(0, end));
            auto colon = line.find(':');
            if (colon != line.npos && Lower(line.substr(0, colon)) == "set-cookie")
                Put(url, Trim(line.substr(colon + 1)));
            if (end == headers.npos) break;
            headers.remove_prefix(end + 1);
        }
    }
    void Put(std::string_view url, std::string_view raw) {
        auto parts = SplitUrl(url);
        if (parts.host.empty() || raw.size() > 2300 || !SafeText(raw)) return;
        auto semicolon = raw.find(';');
        auto first = Trim(raw.substr(0, semicolon));
        auto eq = first.find('=');
        if (eq == first.npos || eq == 0) return;
        Cookie cookie;
        cookie.name = first.substr(0, eq);
        cookie.value = first.substr(eq + 1);
        cookie.domain = parts.host;
        cookie.path = "/";
        auto slash = parts.path.rfind('/');
        if (slash != parts.path.npos && slash > 0) cookie.path = parts.path.substr(0, slash);
        if (cookie.name.size() > 64 || cookie.value.size() > 2048 ||
            cookie.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.!") !=
                std::string::npos)
            return;
        bool remove = cookie.value.empty();
        while (semicolon != raw.npos) {
            raw.remove_prefix(semicolon + 1);
            semicolon = raw.find(';');
            auto item = Trim(raw.substr(0, semicolon));
            auto at = item.find('=');
            auto name = Lower(Trim(item.substr(0, at)));
            auto value = at == item.npos ? std::string_view{} : Trim(item.substr(at + 1));
            if (name == "domain") {
                auto domain = Lower(value);
                if (domain.starts_with('.')) domain.erase(0, 1);
                if (domain.empty() || domain.find('.') == domain.npos || !DomainMatches(parts.host, domain)) return;
                cookie.domain = domain;
                cookie.host_only = false;
            } else if (name == "path" && value.starts_with('/'))
                cookie.path = value;
            else if (name == "max-age" && (value == "0" || value.starts_with('-')))
                remove = true;
        }
        if (cookie.domain.size() > 128 || cookie.path.size() > 128) return;
        Cookie* target = nullptr;
        size_t total = 0;
        for (auto& c : cookies_) {
            if (c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path) {
                target = &c;
                continue;
            }
            total += c.name.size() + c.value.size() + c.domain.size() + c.path.size();
        }
        if (remove) {
            if (target) *target = {};
            return;
        }
        if (total + cookie.name.size() + cookie.value.size() + cookie.domain.size() + cookie.path.size() > 3072) return;
        if (!target)
            for (auto& c : cookies_)
                if (c.name.empty()) {
                    target = &c;
                    break;
                }
        if (target) *target = std::move(cookie);
    }
    std::string MusicToken(std::string_view name) const {
        for (const auto& c : cookies_)
            if (c.name == name && DomainMatches("music.163.com", c.domain)) return c.value;
        return {};
    }

   private:
    struct Cookie {
        std::string name, value, domain, path;
        bool host_only{true};
    };
    std::array<Cookie, 24> cookies_{};
};
class WebRequest {
   public:
    explicit WebRequest(micropixel::Application& app) : app_(app) {}
    bool Start(std::string_view url, std::string_view headers, std::string_view body = {}, bool post = false,
               uint32_t receive_timeout_ms = 10000) {
        request_.Close();
        error.clear();
        response.clear();
        response_headers.clear();
        status = 0;
        auto result = app_.network().Exchange(post ? micropixel::HttpMethod::kPost : micropixel::HttpMethod::kGet, url,
                                              headers, {reinterpret_cast<const uint8_t*>(body.data()), body.size()},
                                              262144, {.receive_timeout_ms = receive_timeout_ms});
        if (!result) {
            error = "网络服务不支持认证请求";
            return false;
        }
        request_ = std::move(*result);
        return true;
    }
    bool Poll() {
        if (!request_.valid()) return false;
        auto state = request_.State();
        if (state && state->phase == micropixel::HttpPhase::kPending) return false;
        if (!state || state->phase == micropixel::HttpPhase::kFailed)
            error = state ? "网络请求失败：" + std::string(state->message) : "网络状态查询失败";
        else {
            auto meta = request_.Metadata();
            if (!meta)
                error = "无法读取响应信息";
            else {
                status = meta->status_code;
                response.resize(state->body_length);
                response_headers.resize(meta->headers_length);
                if (!Read(false, {reinterpret_cast<uint8_t*>(response.data()), response.size()}) ||
                    !Read(true, {reinterpret_cast<uint8_t*>(response_headers.data()), response_headers.size()}))
                    error = "响应读取失败";
            }
        }
        request_.Close();
        return true;
    }
    void Cancel() { request_.Close(); }
    bool busy() const { return request_.valid(); }
    uint16_t status{};
    std::string response, response_headers, error;

   private:
    bool Read(bool headers, std::span<uint8_t> bytes) {
        uint32_t offset = 0;
        while (offset < bytes.size()) {
            auto destination = bytes.subspan(offset, std::min<size_t>(4096, bytes.size() - offset));
            auto n = headers ? request_.ReadHeaders(offset, destination) : request_.Read(offset, destination);
            if (!n || !*n) return false;
            offset += *n;
        }
        return true;
    }
    micropixel::Application& app_;
    micropixel::HttpRequest request_;
};
}  // namespace netease
