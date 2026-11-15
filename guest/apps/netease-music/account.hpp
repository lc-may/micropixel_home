// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "json.hpp"
#include "web.hpp"
namespace netease {
class Account {
   public:
    explicit Account(micropixel::Application& app) : app_(app) {
        fixture_ = app.launch_arguments().FindValue("--test-origin") != nullptr;
        auto size = app.storage().GetBytesSize(Key());
        if (size && *size > 0 && *size <= 3072) {
            std::string saved(*size, 0);
            auto got = app.storage().GetBytes(Key(), reinterpret_cast<uint8_t*>(saved.data()), saved.size());
            if (got && *got == saved.size()) {
                std::string_view remaining = saved;
                while (!remaining.empty()) {
                    auto end = remaining.find(';');
                    auto item = Trim(remaining.substr(0, end));
                    auto eq = item.find('=');
                    auto name = item.substr(0, eq);
                    if (name == "MUSIC_U" || name == "__csrf" || name == "NETEASE_USER_ID")
                        jar.Put("https://music.163.com/", std::string(item) + "; Path=/; Secure");
                    if (end == remaining.npos) break;
                    remaining.remove_prefix(end + 1);
                }
                remember_ = !jar.MusicToken("MUSIC_U").empty();
            }
        }
    }
    bool HasToken() const { return !jar.MusicToken("MUSIC_U").empty(); }
    bool verified() const { return verified_; }
    bool remember() const { return remember_; }
    std::string Csrf() const { return jar.MusicToken("__csrf"); }
    bool SetRemember(bool enabled) {
        remember_ = enabled;
        if (!enabled) {
            auto removed = app_.storage().Remove(Key());
            return removed.has_value() || removed.error().code() == micropixel::ErrorCode::kNotFound;
        }
        return !verified_ || Save();
    }
    bool AcceptProfile(const Json& json) {
        auto profile = json.Field(0, "profile");
        auto id = json.Number(json.Field(json.Field(0, "account"), "id"));
        auto profile_id = json.Number(json.Field(profile, "userId"));
        auto name = json.String(json.Field(profile, "nickname"), 128);
        if (id <= 0 || (profile_id && profile_id != id) || name.empty() || !HasToken()) return false;
        user_id = id;
        nickname = std::move(name);
        avatar_url = json.String(json.Field(profile, "avatarUrl"));
        signature = json.String(json.Field(profile, "signature"), 256);
        verified_ = true;
        return true;
    }
    bool Save() {
        if (!remember_ || !verified_) return true;
        std::string saved;
        for (auto name : {"MUSIC_U", "__csrf", "NETEASE_USER_ID"}) {
            auto value = jar.MusicToken(name);
            if (value.empty()) continue;
            if (!saved.empty()) saved += "; ";
            saved += std::string(name) + '=' + value;
        }
        auto result = app_.storage().SetBytes(Key(), reinterpret_cast<const uint8_t*>(saved.data()), saved.size());
        return result.has_value();
    }
    void Clear() {
        jar.Clear();
        nickname.clear();
        user_id = 0;
        avatar_url.clear();
        signature.clear();
        verified_ = false;
        remember_ = false;
        (void)app_.storage().Remove(Key());
    }
    CookieJar jar;
    std::string nickname, avatar_url, signature;
    uint64_t user_id{};

   private:
    const char* Key() const { return fixture_ ? "ncm.test.auth" : "ncm.auth"; }
    micropixel::Application& app_;
    bool fixture_{}, verified_{}, remember_{};
};
}  // namespace netease
