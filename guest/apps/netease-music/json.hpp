// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sdk/fixed_string.hpp"
namespace netease {
// Bounded, read-only JSON tree. Indices survive vector growth; text is owned by the caller.
class Json {
   public:
    enum Kind { kInvalid, kObject, kArray, kString, kNumber, kLiteral };
    struct Token {
        Kind kind{};
        uint32_t begin{}, end{}, next{};
    };
    explicit Json(std::string_view text, uint32_t token_limit = 4096)
        : text_(text), token_limit_(std::min<uint32_t>(token_limit, 32768)) {
        if (text.size() > 262144) return;
        valid_ = Parse(0);
        Space();
        valid_ = valid_ && offset_ == text.size();
    }
    bool valid() const { return valid_; }
    Kind Type(int i) const { return i >= 0 && size_t(i) < tokens_.size() ? tokens_[i].kind : kInvalid; }
    int Field(int object, std::string_view key) const {
        if (Type(object) != kObject) return -1;
        for (uint32_t i = object + 1; i < tokens_[object].next;) {
            uint32_t value = i + 1;
            if (String(i) == key) return value;
            i = tokens_[value].next;
        }
        return -1;
    }
    int At(int array, uint32_t item) const {
        if (Type(array) != kArray) return -1;
        for (uint32_t i = array + 1; i < tokens_[array].next; i = tokens_[i].next)
            if (item-- == 0) return i;
        return -1;
    }
    uint32_t Count(int array) const {
        if (Type(array) != kArray) return 0;
        uint32_t n = 0;
        for (uint32_t i = array + 1; i < tokens_[array].next; i = tokens_[i].next) ++n;
        return n;
    }
    int First(int array) const {
        return Type(array) == kArray && tokens_[array].next > uint32_t(array + 1) ? array + 1 : -1;
    }
    int Next(int array, int item) const {
        if (Type(array) != kArray || item <= array || size_t(item) >= tokens_.size() ||
            uint32_t(item) >= tokens_[array].next)
            return -1;
        return tokens_[item].next < tokens_[array].next ? int(tokens_[item].next) : -1;
    }
    bool Boolean(int i) const {
        return Type(i) == kLiteral && text_.substr(tokens_[i].begin, tokens_[i].end - tokens_[i].begin) == "true";
    }
    int64_t Number(int i) const {
        if (Type(i) != kNumber) return 0;
        auto s = text_.substr(tokens_[i].begin, tokens_[i].end - tokens_[i].begin);
        bool negative = s.starts_with('-');
        if (negative) s.remove_prefix(1);
        uint64_t n = 0;
        for (char c : s) {
            if (c < '0' || c > '9' || n > (9007199254740991ULL - uint64_t(c - '0')) / 10) return 0;
            n = n * 10 + c - '0';
        }
        return negative ? -int64_t(n) : int64_t(n);
    }
    std::string String(int i, uint32_t limit = 2048) const {
        if (Type(i) != kString) return {};
        std::string result;
        auto end = tokens_[i].end;
        for (uint32_t p = tokens_[i].begin; p < end;) {
            char c = text_[p++];
            if (c != '\\')
                result += c;
            else {
                c = text_[p++];
                if (c == 'u') {
                    uint32_t cp = Hex4(p);
                    p += 4;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        p += 2;
                        auto low = Hex4(p);
                        p += 4;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    }
                    Utf8(result, cp);
                } else {
                    switch (c) {
                        case 'n':
                            result += '\n';
                            break;
                        case 'r':
                            result += '\r';
                            break;
                        case 't':
                            result += '\t';
                            break;
                        case 'b':
                            result += '\b';
                            break;
                        case 'f':
                            result += '\f';
                            break;
                        default:
                            result += c;
                    }
                }
            }
            if (result.size() > limit) return {};
        }
        return result;
    }

   private:
    static int Hex(char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    }
    uint32_t Hex4(uint32_t p) const {
        uint32_t n = 0;
        for (unsigned j = 0; j < 4; ++j) n = n * 16 + Hex(text_[p + j]);
        return n;
    }
    static void Utf8(std::string& s, uint32_t cp) {
        if (cp < 128)
            s += char(cp);
        else if (cp < 2048) {
            s += char(0xc0 | (cp >> 6));
            s += char(0x80 | (cp & 63));
        } else if (cp < 65536) {
            s += char(0xe0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 63));
            s += char(0x80 | (cp & 63));
        } else {
            s += char(0xf0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 63));
            s += char(0x80 | ((cp >> 6) & 63));
            s += char(0x80 | (cp & 63));
        }
    }
    void Space() {
        while (offset_ < text_.size() &&
               (text_[offset_] == ' ' || text_[offset_] == '\n' || text_[offset_] == '\r' || text_[offset_] == '\t'))
            ++offset_;
    }
    bool Take(char c) {
        Space();
        if (offset_ == text_.size() || text_[offset_] != c) return false;
        ++offset_;
        return true;
    }
    bool Parse(unsigned depth) {
        Space();
        if (depth > 24 || offset_ == text_.size() || tokens_.size() >= token_limit_) return false;
        const uint32_t index = tokens_.size();
        tokens_.push_back({});
        const char c = text_[offset_];
        if (c == '{' || c == '[') {
            tokens_[index].kind = c == '{' ? kObject : kArray;
            ++offset_;
            Space();
            const char close = c == '{' ? '}' : ']';
            if (offset_ < text_.size() && text_[offset_] == close)
                ++offset_;
            else
                for (;;) {
                    if (c == '{') {
                        Space();
                        if (offset_ == text_.size() || text_[offset_] != '"' || !Parse(depth + 1) || !Take(':'))
                            return false;
                    }
                    if (!Parse(depth + 1)) return false;
                    if (Take(close)) break;
                    if (!Take(',')) return false;
                }
        } else if (c == '"') {
            tokens_[index].kind = kString;
            tokens_[index].begin = ++offset_;
            bool closed = false;
            while (offset_ < text_.size()) {
                unsigned char ch = text_[offset_++];
                if (ch == '"') {
                    closed = true;
                    break;
                }
                if (ch < 32) return false;
                if (ch != '\\') continue;
                if (offset_ == text_.size()) return false;
                char esc = text_[offset_++];
                if (esc == 'u') {
                    if (offset_ + 4 > text_.size()) return false;
                    for (unsigned j = 0; j < 4; ++j)
                        if (Hex(text_[offset_ + j]) < 0) return false;
                    auto cp = Hex4(offset_);
                    offset_ += 4;
                    if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (offset_ + 6 > text_.size() || text_.substr(offset_, 2) != "\\u") return false;
                        offset_ += 2;
                        for (unsigned j = 0; j < 4; ++j)
                            if (Hex(text_[offset_ + j]) < 0) return false;
                        auto low = Hex4(offset_);
                        offset_ += 4;
                        if (low < 0xdc00 || low > 0xdfff) return false;
                    }
                } else if (std::string_view("\"\\/bfnrt").find(esc) == std::string_view::npos)
                    return false;
            }
            if (!closed) return false;
            tokens_[index].end = offset_ - 1;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            tokens_[index].kind = kNumber;
            tokens_[index].begin = offset_;
            if (c == '-') ++offset_;
            if (offset_ == text_.size()) return false;
            if (text_[offset_] == '0')
                ++offset_;
            else {
                if (text_[offset_] < '1' || text_[offset_] > '9') return false;
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
            }
            if (offset_ < text_.size() && text_[offset_] == '.') {
                ++offset_;
                auto start = offset_;
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
                if (start == offset_) return false;
            }
            if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
                ++offset_;
                if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) ++offset_;
                auto start = offset_;
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
                if (start == offset_) return false;
            }
            tokens_[index].end = offset_;
        } else {
            tokens_[index].kind = kLiteral;
            tokens_[index].begin = offset_;
            bool found = false;
            for (auto literal : {"true", "false", "null"}) {
                auto s = std::string_view(literal);
                if (text_.substr(offset_, s.size()) == s) {
                    offset_ += s.size();
                    found = true;
                    break;
                }
            }
            if (!found) return false;
            tokens_[index].end = offset_;
        }
        tokens_[index].next = tokens_.size();
        return true;
    }
    std::string_view text_;
    uint32_t token_limit_;
    std::vector<Token> tokens_;
    uint32_t offset_{};
    bool valid_{};
};
inline std::string Decimal(uint64_t n) {
    micropixel::FixedString<24> text;
    text.AppendUint(n);
    return text.c_str();
}
inline std::string Quote(std::string_view s) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    return out + '"';
}
}  // namespace netease
