// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <string>

#include "abi/micropixel_abi.h"
#include "apps/netease-music/account.hpp"
#include "apps/netease-music/login_protocol.hpp"
#include "sdk/micropixel.hpp"
using namespace micropixel;
using namespace micropixel::literals;
int main() {
    Application app;
    Assert(netease::WechatPollingOrigin("var usenewdomain = '1' * 1 || 0;") == "https://lp.open.weixin.qq.com",
           "new WeChat polling endpoint");
    Assert(netease::WechatPollingOrigin("var usenewdomain = '0' * 1 || 0;") == "https://long.open.weixin.qq.com",
           "legacy WeChat polling endpoint");
    Assert(netease::TrustedAuthUrl("https://lp.open.weixin.qq.com/connect/l/qrconnect"), "new endpoint allowlist");
    Assert(!netease::TrustedAuthUrl("https://lp.open.weixin.qq.com.evil.test/"), "fake polling endpoint accepted");
    netease::CookieJar jar;
    jar.Receive("https://music.163.com/back/weichat",
                "Set-Cookie: MUSIC_U=fixture-session; Domain=.163.com; Path=/; Secure; HttpOnly\r\nSet-Cookie: "
                "__csrf=fixture-csrf; Path=/; Secure\r\n");
    jar.Receive("https://open.weixin.qq.com/connect/qrconnect",
                "Set-Cookie: WX_TOKEN=wechat-only; Domain=.weixin.qq.com; Path=/connect; Secure\r\n");
    Assert(jar.For("https://music.163.com/weapi/search/get").find("fixture-session") != std::string::npos,
           "music cookie missing");
    Assert(jar.For("https://open.weixin.qq.com/connect/qrcode/test").find("MUSIC_U") == std::string::npos,
           "music cookie leaked to WeChat");
    Assert(jar.For("https://music.163.com/").find("WX_TOKEN") == std::string::npos, "WeChat cookie leaked to music");
    Assert(jar.For("http://music.163.com/").empty(), "secure cookie leaked to HTTP");
    Assert(jar.For("https://open.weixin.qq.com/connected").empty(), "cookie path boundary");
    jar.Put("https://music.163.com/", "EVIL=bad; Domain=.com; Path=/");
    jar.Put("https://open.weixin.qq.com/", "MUSIC_U=stolen; Domain=music.163.com; Path=/");
    jar.Put("https://music.163.com/", "INJECT=a\r\nX-Evil: yes; Path=/");
    Assert(jar.MusicToken("MUSIC_U") == "fixture-session", "cross-domain cookie overwrite");
    Assert(jar.For("https://music.163.com/").find("EVIL") == std::string::npos &&
               jar.For("https://music.163.com/").find("INJECT") == std::string::npos,
           "malicious cookie accepted");
    jar.Put("https://music.163.com/", "MUSIC_U=; Domain=.163.com; Path=/; Max-Age=0");
    Assert(jar.MusicToken("MUSIC_U").empty(), "cookie deletion");
    for (auto bad : {"http://music.163.com/back", "https://music.163.com.evil.com/", "https://music.163.com@evil.com/",
                     "https://music.163.com:444/", "https://open.weixin.qq.com\\@evil/"})
        Assert(!netease::TrustedAuthUrl(bad), "untrusted auth URL accepted");
    Assert(netease::Query(
               "https://open.weixin.qq.com/?redirect_uri=https%3A%2F%2Fmusic.163.com%2Fback%2Fweichat&state=a%2Bb",
               "redirect_uri") == "https://music.163.com/back/weichat",
           "redirect URI decoding");
    Assert(netease::Query("https://open.weixin.qq.com/?state=abc#wechat_redirect", "state") == "abc",
           "OAuth URL fragment");
    Assert(netease::UrlDecode("bad%0d%0a").empty(), "URL header injection");
    netease::Account account(app);
    account.Clear();
    account.jar.Put("https://music.163.com/", "MUSIC_U=contract-session; Path=/; Secure");
    account.jar.Put("https://music.163.com/", "__csrf=contract-csrf; Path=/; Secure");
    netease::Json profile("{\"code\":200,\"account\":{\"id\":42},\"profile\":{\"nickname\":\"测试账号\"}}");
    Assert(account.AcceptProfile(profile), "account profile acceptance");
    Assert(account.user_id == 42 && account.nickname == "测试账号", "verified profile identity");
    netease::Json mismatch("{\"code\":200,\"account\":{\"id\":42},\"profile\":{\"userId\":43,\"nickname\":\"other\"}}");
    Assert(!account.AcceptProfile(mismatch), "mismatched profile accepted");
    Assert(account.SetRemember(true), "remember account");
    netease::Account restored(app);
    Assert(restored.HasToken() && !restored.verified(), "restored account must be reverified");
    Assert(restored.SetRemember(false), "forget account");
    netease::Account forgotten(app);
    Assert(!forgotten.HasToken(), "forgotten credentials remained");
    account.Clear();
    const char* origin = app.launch_arguments().FindValue("--test-origin");
    Assert(origin, "test origin missing");
    auto base = std::string(origin);
    auto started = app.network().Exchange(HttpMethod::kGet, base + "/__contract/exchange", "X-Contract: yes\r\n");
    Assert(started.has_value(), "raw HTTP exchange");
    auto http = std::move(*started);
    Assert(!app.network().Exchange(HttpMethod::kGet, base, {}, {}, 4096, {.receive_timeout_ms = 0}),
           "zero receive timeout");
    Assert(!app.network().Exchange(HttpMethod::kGet, base, {}, {}, 4096, {.receive_timeout_ms = 60001}),
           "unbounded receive timeout");
    unsigned stage = 0, ticks = 0;
    auto timer = app.timers().Every(20_ms).value();
    app.Run([&](const Event& event) -> EventResult {
        if (event.type() == EventType::kStop) return EventResult::kExit;
        if (!event.TimerFrom(timer)) return EventResult::kContinue;
        Assert(++ticks < 500, "auth contract timeout");
        auto state = http.State();
        Assert(state.has_value(), "exchange state");
        if (state->phase == HttpPhase::kPending) return EventResult::kContinue;
        if (stage == 4)
            Assert(state->phase == HttpPhase::kFailed, "per-request short timeout ignored");
        else
            Assert(state->phase == HttpPhase::kComplete, "exchange transport");
        if (stage == 0) {
            auto meta = http.Metadata();
            Assert(meta && meta->status_code == 302 && meta->headers_length > 0, "raw HTTP status");
            std::string headers(meta->headers_length, 0);
            auto read = http.ReadHeaders(0, {reinterpret_cast<uint8_t*>(headers.data()), headers.size()});
            Assert(read && *read == headers.size(), "response headers read");
            Assert(netease::Header(headers, "location") == "https://example.com/never-follow", "redirect metadata");
            Assert(headers.find("a=1") != headers.npos && headers.find("b=2") != headers.npos,
                   "repeated Set-Cookie lost");
            std::array<uint8_t, 4097> oversized{};
            Assert(!http.ReadHeaders(0, oversized), "header read capacity");
            std::array<uint8_t, 8> bytes{};
            Assert(!http.ReadHeaders(meta->headers_length + 1, bytes), "header offset bounds");
        } else if (stage < 4) {
            std::vector<uint8_t> bytes(state->body_length);
            uint32_t offset = 0;
            while (offset < bytes.size()) {
                auto part = std::span<uint8_t>(bytes).subspan(offset, std::min<size_t>(4096, bytes.size() - offset));
                auto read = http.Read(offset, part);
                Assert(read && *read > 0, "encoded image read");
                offset += *read;
            }
            auto texture = app.resources().DecodeTexture(bytes);
            if (stage == 1) Assert(texture && texture->width() == 32 && texture->height() == 32, "PNG decode");
            if (stage == 2) Assert(texture && texture->width() == 470 && texture->height() == 470, "JPEG decode");
            if (stage == 3) Assert(!texture, "oversized encoded image accepted");
        }
        http.Close();
        ++stage;
        if (stage == 4 || stage == 5) {
            auto next = app.network().Exchange(HttpMethod::kGet, base + "/__contract/delay", {}, {}, 4096,
                                               {.receive_timeout_ms = stage == 4 ? 100U : 1000U});
            Assert(next.has_value(), "timed exchange start");
            http = std::move(*next);
            return EventResult::kContinue;
        }
        if (stage == 6) {
            micropixel_service_info_t resource{};
            Assert(micropixel_service_open(MICROPIXEL_SERVICE_RESOURCE, MICROPIXEL_INTERFACE_VERSION(1, 1), &resource,
                                           sizeof(resource)) == 0,
                   "resource 1.1");
            micropixel_texture_decode_request_t bad{sizeof(bad), 0, 0xfffffff0U, 32};
            micropixel_texture_info_t out{};
            uint32_t size = 0;
            auto status = micropixel_service_call(resource.service_handle, MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE,
                                                  reinterpret_cast<const uint8_t*>(&bad), sizeof(bad),
                                                  reinterpret_cast<uint8_t*>(&out), sizeof(out), &size);
            Assert(status == MICROPIXEL_STATUS_INVALID_MEMORY, "invalid image Guest range");
            micropixel_service_info_t network{};
            Assert(micropixel_service_open(MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_INTERFACE_VERSION(1, 2), &network,
                                           sizeof(network)) == 0,
                   "Network 1.2 open");
            micropixel_http_timed_start_request_t invalid{};
            invalid.request.size = sizeof(invalid);
            invalid.request.method = MICROPIXEL_HTTP_GET;
            invalid.receive_timeout_ms = UINT32_MAX;
            uint32_t response_bytes = 0;
            micropixel_handle_response_t reply{};
            Assert(micropixel_service_call(network.service_handle, MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE,
                                           reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid),
                                           reinterpret_cast<uint8_t*>(&reply), sizeof(reply),
                                           &response_bytes) == MICROPIXEL_STATUS_INVALID_ARGUMENT,
                   "Host receive timeout validation");
            app.log().Info("AUTH_CONTRACT PASS: cookie isolation persistence HTTP metadata image bounds");
            return EventResult::kExit;
        }
        const char* path = stage == 1   ? "/__contract/png"
                           : stage == 2 ? "/__wx/connect/qrcode/fixture"
                                        : "/__contract/oversize";
        auto next = app.network().Exchange(HttpMethod::kGet, base + path);
        Assert(next.has_value(), "next exchange");
        http = std::move(*next);
        return EventResult::kContinue;
    });
    return 0;
}
