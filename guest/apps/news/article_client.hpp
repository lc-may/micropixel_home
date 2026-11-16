// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "news_client.hpp"

namespace news {
inline constexpr size_t MaxArticleText = 32768;
struct Document {
    std::string text, source;
    bool truncated{}, media_only{};
};
inline bool AsciiSpace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
inline size_t FindNoCase(std::string_view text, std::string_view needle, size_t start = 0) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
    for (size_t at = start; at + needle.size() <= text.size(); ++at) {
        bool same = true;
        for (size_t i = 0; i < needle.size(); ++i)
            if (lower(text[at + i]) != lower(needle[i])) {
                same = false;
                break;
            }
        if (same) return at;
    }
    return text.npos;
}
inline void AppendCodepoint(std::string& out, uint32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || (!cp)) return;
    if (cp < 128)
        out += char(cp);
    else if (cp < 2048) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 63));
    } else if (cp < 65536) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    } else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    }
}
inline int HexDigit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
inline size_t Utf8Prefix(std::string_view text, size_t limit) {
    size_t end = std::min(text.size(), limit);
    while (end && end < text.size() && (uint8_t(text[end]) & 0xc0) == 0x80) --end;
    return end;
}
// Decode the page's quoted article data. This never evaluates JavaScript.
inline bool QuotedData(std::string_view html, size_t at, std::string& out) {
    while (at < html.size() && AsciiSpace(html[at])) ++at;
    if (at >= html.size() || (html[at] != '\'' && html[at] != '"')) return false;
    char quote = html[at++];
    out.clear();
    while (at < html.size() && out.size() <= MaxBody) {
        char c = html[at++];
        if (c == quote) return ui::ValidUtf8(out);
        if (c != '\\') {
            out += c;
            continue;
        }
        if (at >= html.size()) return false;
        c = html[at++];
        if (c == 'n')
            out += '\n';
        else if (c == 'r')
            out += '\r';
        else if (c == 't')
            out += '\t';
        else if (c == '\n')
            continue;
        else if (c == '\r') {
            if (at < html.size() && html[at] == '\n') ++at;
        } else if (c == 'u' || c == 'x') {
            unsigned digits = c == 'u' ? 4 : 2;
            if (at + digits > html.size()) return false;
            uint32_t cp = 0;
            for (unsigned i = 0; i < digits; ++i) {
                auto n = HexDigit(html[at++]);
                if (n < 0) return false;
                cp = cp * 16 + unsigned(n);
            }
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (at + 6 > html.size() || html.substr(at, 2) != "\\u") return false;
                at += 2;
                uint32_t low = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    auto n = HexDigit(html[at++]);
                    if (n < 0) return false;
                    low = low * 16 + unsigned(n);
                }
                if (low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff)
                return false;
            AppendCodepoint(out, cp);
        } else if (c == '\\' || c == '\'' || c == '"' || c == '/')
            out += c;
        else
            return false;
    }
    return false;
}
inline std::string DecodeEntities(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out += text[i++];
            continue;
        }
        auto end = text.find(';', i + 1);
        if (end == text.npos || end - i > 16) {
            out += text[i++];
            continue;
        }
        auto name = text.substr(i + 1, end - i - 1);
        uint32_t cp = 0;
        if (name == "amp")
            cp = '&';
        else if (name == "lt")
            cp = '<';
        else if (name == "gt")
            cp = '>';
        else if (name == "quot")
            cp = '"';
        else if (name == "apos" || name == "#39")
            cp = '\'';
        else if (name == "nbsp" || name == "ensp" || name == "emsp")
            cp = ' ';
        else if (name == "ldquo")
            cp = 0x201c;
        else if (name == "rdquo")
            cp = 0x201d;
        else if (name == "lsquo")
            cp = 0x2018;
        else if (name == "rsquo")
            cp = 0x2019;
        else if (name == "mdash")
            cp = 0x2014;
        else if (name == "ndash")
            cp = 0x2013;
        else if (name == "hellip")
            cp = 0x2026;
        else if (name == "middot")
            cp = 0xb7;
        else if (name.starts_with('#')) {
            name.remove_prefix(1);
            unsigned base = 10;
            if (name.starts_with('x') || name.starts_with('X')) {
                base = 16;
                name.remove_prefix(1);
            }
            bool valid = !name.empty();
            for (char c : name) {
                int n = HexDigit(c);
                if (n < 0 || unsigned(n) >= base || cp > (0x10ffff - unsigned(n)) / base) {
                    valid = false;
                    break;
                }
                cp = cp * base + unsigned(n);
            }
            if (!valid || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0;
        }
        if (cp) {
            AppendCodepoint(out, cp);
            i = end + 1;
        } else
            out += text[i++];
    }
    return out;
}
inline std::string HtmlText(std::string_view html, bool& truncated) {
    std::string out;
    auto paragraph = [&] {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (!out.empty() && out.back() != '\n') out += '\n';
    };
    for (size_t at = 0; at < html.size() && out.size() <= MaxArticleText;) {
        if (html.substr(at).starts_with("<!--")) {
            auto end = html.find("-->", at + 4);
            if (end == html.npos) break;
            at = end + 3;
            continue;
        }
        if (html.substr(at).starts_with("[!--begin:")) {
            auto end = html.find("[!--end:", at + 9);
            if (end == html.npos) break;
            end = html.find("--]", end);
            if (end == html.npos) break;
            at = end + 3;
            continue;
        }
        if (html[at] == '<') {
            size_t end = at + 1;
            char quote = 0;
            for (; end < html.size(); ++end) {
                char c = html[end];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '\'' || c == '"')
                    quote = c;
                else if (c == '>')
                    break;
            }
            if (end == html.size()) break;
            auto tag = html.substr(at + 1, end - at - 1);
            while (!tag.empty() && (AsciiSpace(tag.front()) || tag.front() == '/')) tag.remove_prefix(1);
            auto stop = tag.find_first_of(" /\t\r\n");
            std::string name(tag.substr(0, stop));
            for (auto& c : name)
                if (c >= 'A' && c <= 'Z') c += 32;
            if (name == "script" || name == "style" || name == "iframe" || name == "object" || name == "svg") {
                auto closing = FindNoCase(html, "</" + name, end + 1);
                if (closing == html.npos) break;
                auto closeend = html.find('>', closing);
                if (closeend == html.npos) break;
                at = closeend + 1;
                continue;
            }
            if (name == "p" || name == "br" || name == "div" || name == "li" || name == "h1" || name == "h2" ||
                name == "h3" || name == "blockquote")
                paragraph();
            at = end + 1;
            continue;
        }
        auto end = html.find('<', at);
        auto marker = html.find("[!--begin:", at);
        end = std::min(end == html.npos ? html.size() : end, marker == html.npos ? html.size() : marker);
        auto text = DecodeEntities(html.substr(at, end - at));
        for (uint8_t c : text) {
            if (c <= 32 || c == 127) {
                if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
            } else
                out += char(c);
        }
        at = end;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    if (out.size() > MaxArticleText) {
        out.resize(Utf8Prefix(out, MaxArticleText));
        truncated = true;
    }
    return ui::ValidUtf8(out) ? out : std::string{};
}
inline bool ParseDocument(std::string_view html, Document& output) {
    if (html.size() > MaxBody) return false;
    Document doc;
    doc.source = "央视网";
    // Older and newer article templates both occur in the live feed.
    auto content = FindNoCase(html, "id=\"content_area\"");
    if (content == html.npos) content = FindNoCase(html, "id='content_area'");
    if (content != html.npos) {
        auto start = html.find('>', content);
        size_t end = start, depth = 1;
        if (start != html.npos) {
            ++start;
            end = start;
            while (depth && end < html.size()) {
                auto open = FindNoCase(html, "<div", end), close = FindNoCase(html, "</div", end);
                if (close == html.npos) break;
                if (open != html.npos && open < close) {
                    ++depth;
                    end = open + 4;
                } else {
                    if (--depth == 0) {
                        doc.text = HtmlText(html.substr(start, close - start), doc.truncated);
                        break;
                    }
                    end = close + 5;
                }
            }
        }
    }
    if (doc.text.empty()) {
        auto var = html.find("var contentdate");
        if (var == html.npos) var = html.find("var contentData");
        if (var != html.npos) {
            auto eq = html.find('=', var);
            std::string markup;
            if (eq != html.npos && eq - var < 80 && QuotedData(html, eq + 1, markup))
                doc.text = HtmlText(markup, doc.truncated);
        }
    }
    auto source = FindNoCase(html, "name=\"source\"");
    if (source != html.npos) {
        auto end = html.find('>', source);
        auto value = FindNoCase(html, "content=", source);
        if (value != html.npos && value < end) {
            value += 8;
            if (value < html.size() && (html[value] == '\'' || html[value] == '"')) {
                auto close = html.find(html[value], value + 1);
                if (close != html.npos && close < end) {
                    auto name = DecodeEntities(html.substr(value + 1, close - value - 1));
                    if (ValidTitle(name) && name.size() < 64) doc.source = name;
                }
            }
        }
    }
    doc.media_only =
        doc.text.empty() && (html.find("htmlVideoCode") != html.npos || html.find("newPlayer") != html.npos);
    if (doc.text.empty() && !doc.media_only) return false;
    output = std::move(doc);
    return true;
}
class ArticleClient final {
   public:
    bool Start(const Application& app, std::string_view path) {
        Cancel();
        if (!NewsClient::ValidCctvPath(path)) return false;
        const char* origin = app.launch_arguments().FindValue("--news-article-origin");
        std::string url = origin ? origin : "https://news.cctv.com/";
        if (!url.ends_with('/')) url += '/';
        url += path;
        auto opened = app.network().Exchange(HttpMethod::kGet, url,
                                             "User-Agent: Mozilla/5.0 MicroPixel/1.0\r\nAccept: text/html\r\n", {},
                                             MaxBody, {.receive_timeout_ms = 15000});
        if (!opened) return false;
        request_.emplace(std::move(*opened));
        return true;
    }
    int Poll(Document& output) {
        if (!request_) return -1;
        auto state = request_->State();
        if (!state) return -1;
        if (state->phase == HttpPhase::kPending) return 0;
        auto meta = request_->Metadata();
        if (state->phase != HttpPhase::kComplete || !meta || meta->status_code != 200 || !state->body_length ||
            state->body_length > MaxBody)
            return -1;
        std::string html(state->body_length, '\0');
        for (uint32_t at = 0; at < html.size();) {
            auto n = std::min<uint32_t>(4096, html.size() - at);
            auto got = request_->Read(at, {reinterpret_cast<uint8_t*>(html.data()) + at, n});
            if (!got || *got != n) return -1;
            at += n;
        }
        return ParseDocument(html, output) ? 1 : -1;
    }
    void Cancel() { request_.reset(); }

   private:
    std::optional<HttpRequest> request_;
};
}  // namespace news
