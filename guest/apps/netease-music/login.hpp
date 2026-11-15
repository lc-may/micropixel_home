// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "account.hpp"
#include "login_protocol.hpp"
#include "weapi.hpp"
namespace netease {
enum class LoginPhase { kIdle, kLoading, kWaiting, kScanned, kAuthenticated, kExpired, kCancelled, kError };
class Login {
   public:
    Login(micropixel::Application& app, Account& account) : app_(app), account_(account), http_(app) {
        if (const char* origin = app.launch_arguments().FindValue("--test-origin")) {
            std::string_view url = origin;
            if (url.starts_with("http://127.0.0.1:") && url.find_first_of("/@?#", 7) == url.npos) fixture_ = url;
        }
    }
    void Start() {
        Cancel();
        deadline_ = Now() + 180000;
        callback_.clear();
        state_.clear();
        uuid_.clear();
        last_ = 0;
        poll_origin_ = "https://long.open.weixin.qq.com";
        phase = LoginPhase::kLoading;
        message = "正在获取微信二维码…";
        Request("https://music.163.com/api/sns/authorize?snsType=10&clientType=web2&callbackType=Login&forcelogin=true",
                Kind::kAuthorize);
    }
    void Verify() {
        Cancel();
        phase = LoginPhase::kLoading;
        message = "正在验证登录状态…";
        VerifyRequest();
    }
    void Cancel() {
        http_.Cancel();
        kind_ = Kind::kNone;
        next_poll_ = 0;
    }
    void Logout() {
        Cancel();
        account_.Clear();
        phase = LoginPhase::kIdle;
        message = "已退出登录";
    }
    bool Poll() {
        auto now = Now();
        if ((phase == LoginPhase::kWaiting || phase == LoginPhase::kScanned || kind_ == Kind::kAuthorize ||
             kind_ == Kind::kPage || kind_ == Kind::kImage) &&
            now >= deadline_) {
            Cancel();
            phase = LoginPhase::kExpired;
            message = "二维码已过期，请刷新";
            return true;
        }
        if (!http_.busy()) {
            if ((phase == LoginPhase::kWaiting || phase == LoginPhase::kScanned) && next_poll_ && now >= next_poll_) {
                next_poll_ = 0;
                Request(poll_origin_ + "/connect/l/qrconnect?uuid=" + UrlEncode(uuid_) +
                            (last_ ? "&last=" + Decimal(last_) : "") + "&_=" + Decimal(now),
                        Kind::kPoll);
            }
            return false;
        }
        if (!http_.Poll()) return false;
        const auto completed = kind_;
        kind_ = Kind::kNone;
        if (!http_.error.empty()) {
            if (completed == Kind::kPoll) {
                app_.log().Info(("NETEASE_LOGIN poll transport error: elapsed_ms=" + Decimal(now - poll_started_) +
                                 " " + http_.error)
                                    .c_str());
                message = phase == LoginPhase::kScanned ? "已扫码，状态查询重试中，请在手机确认"
                                                        : "扫码状态查询失败，正在重试";
                next_poll_ = now + 1000;
                return true;
            }
            Fail(http_.error);
            return true;
        }
        account_.jar.Receive(current_url_, http_.response_headers);
        if (http_.status >= 300 && http_.status < 400) {
            auto location = Header(http_.response_headers, "location");
            if (location.starts_with('/') && !location.starts_with("//"))
                location = std::string(SplitUrl(current_url_).origin) + location;
            if (++redirects_ > 6 || !TrustedAuthUrl(location)) {
                Fail("认证跳转地址无效");
                return true;
            }
            if (completed == Kind::kAuthorize && SplitUrl(location).host == "open.weixin.qq.com") {
                callback_ = Query(location, "redirect_uri");
                state_ = Query(location, "state");
                if (!TrustedAuthUrl(callback_) || SplitUrl(callback_).host != "music.163.com" || state_.empty() ||
                    state_.size() > 512) {
                    Fail("微信授权参数无效");
                    return true;
                }
                Request(location, Kind::kPage, false);
                return true;
            }
            if (completed == Kind::kCallback && account_.HasToken()) {
                VerifyRequest();
                return true;
            }
            Request(location, completed, false);
            return true;
        }
        if (http_.status != 200) {
            Fail("登录服务返回 HTTP " + Decimal(http_.status) + "，请稍后重试");
            return true;
        }
        if (completed == Kind::kAuthorize) {
            Fail("官网认证流程已变化，请稍后重试");
            return true;
        }
        if (completed == Kind::kPage) {
            poll_origin_ = WechatPollingOrigin(http_.response);
            app_.log().Info(("NETEASE_LOGIN polling endpoint: " + poll_origin_).c_str());
            auto at = http_.response.find("/connect/qrcode/");
            if (at == std::string::npos) {
                Fail("微信未返回可用二维码");
                return true;
            }
            at += std::string_view("/connect/qrcode/").size();
            auto end = http_.response.find_first_of("\"'<>? ", at);
            uuid_ = http_.response.substr(at, end - at);
            if (uuid_.empty() || uuid_.size() > 128 ||
                uuid_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                    uuid_.npos) {
                Fail("微信二维码参数无效");
                return true;
            }
            Request("https://open.weixin.qq.com/connect/qrcode/" + uuid_, Kind::kImage);
            return true;
        }
        if (completed == Kind::kImage) {
            auto decoded = app_.resources().DecodeTexture(
                {reinterpret_cast<const uint8_t*>(http_.response.data()), http_.response.size()});
            if (!decoded) {
                Fail("二维码图片无法显示，请刷新");
                return true;
            }
            qr = std::move(*decoded);
            ++qr_generation;
            phase = LoginPhase::kWaiting;
            message = "请用微信扫一扫，在手机上确认登录";
            next_poll_ = now + 1000;
            return true;
        }
        if (completed == Kind::kPoll) {
            auto code = ScriptAssignment(http_.response, "wx_errcode");
            const bool known_code =
                code == "400" || code == "402" || code == "403" || code == "404" || code == "405" || code == "408";
            app_.log().Info(("NETEASE_LOGIN poll response: status=" + (known_code ? code : "invalid") +
                             " elapsed_ms=" + Decimal(now - poll_started_))
                                .c_str());
            if (code == "405") {
                auto token = ScriptAssignment(http_.response, "wx_code");
                if (token.empty() || token.size() > 512 ||
                    token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                        token.npos) {
                    Fail("微信未返回授权凭据");
                    return true;
                }
                phase = LoginPhase::kLoading;
                message = "正在完成网易云授权…";
                Request(callback_ + (callback_.find('?') == std::string::npos ? "?" : "&") +
                            "code=" + UrlEncode(token) + "&state=" + UrlEncode(state_),
                        Kind::kCallback);
                return true;
            }
            if (code == "404") {
                phase = LoginPhase::kScanned;
                message = "扫描成功，请在手机微信中确认";
                last_ = 404;
            } else if (code == "403") {
                phase = LoginPhase::kCancelled;
                message = "你已取消此次登录，可刷新重试";
                return true;
            } else if (code == "400" || code == "402") {
                phase = LoginPhase::kExpired;
                message = "二维码已过期，请刷新";
                return true;
            } else if (code != "408") {
                Fail("微信登录状态无效，请刷新");
                return true;
            }
            if (code == "408")
                message =
                    phase == LoginPhase::kScanned ? "扫描成功，请在手机微信中确认" : "等待微信扫码（状态连接正常）";
            next_poll_ = now + 1000;
            return true;
        }
        if (completed == Kind::kCallback) {
            if (account_.HasToken())
                VerifyRequest();
            else
                Fail("授权尚未完成，可能需要在官网绑定手机号");
            return true;
        }
        if (completed == Kind::kVerify) {
            Json json(http_.response);
            if (!json.valid() || json.Number(json.Field(0, "code")) != 200 || !account_.AcceptProfile(json)) {
                account_.Clear();
                Fail("登录已失效，或该微信尚未关联网易云账号");
                return true;
            }
            phase = LoginPhase::kAuthenticated;
            message = account_.Save() ? "登录成功" : "登录成功，但无法记住登录状态";
            return true;
        }
        return false;
    }
    LoginPhase phase{LoginPhase::kIdle};
    std::string message;
    micropixel::Texture qr;
    uint32_t qr_generation{};

   private:
    enum class Kind { kNone, kAuthorize, kPage, kImage, kPoll, kCallback, kVerify };
    uint64_t Now() const { return app_.clock().Now().microseconds() / 1000; }
    std::string TransportUrl(const std::string& url) const {
        if (fixture_.empty()) return url;
        auto parts = SplitUrl(url);
        auto suffix = url.substr(parts.origin.size());
        return fixture_ +
               (parts.host == "open.weixin.qq.com"        ? "/__wx"
                : parts.host == "lp.open.weixin.qq.com"   ? "/__poll-new"
                : parts.host == "long.open.weixin.qq.com" ? "/__poll"
                                                          : "") +
               suffix;
    }
    void Request(std::string url, Kind kind, bool reset = true, std::string body = {}) {
        if (reset) redirects_ = 0;
        if (!TrustedAuthUrl(url)) {
            Fail("拒绝非官方认证地址");
            return;
        }
        current_url_ = std::move(url);
        kind_ = kind;
        std::string headers =
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/124.0.0.0 "
            "Safari/537.36\r\nReferer: ";
        headers += (kind == Kind::kImage || kind == Kind::kPoll) ? "https://open.weixin.qq.com/\r\n"
                                                                 : "https://music.163.com/\r\n";
        auto cookies = account_.jar.For(current_url_);
        if (!cookies.empty()) headers += "Cookie: " + cookies + "\r\n";
        if (!body.empty()) headers += "Content-Type: application/x-www-form-urlencoded\r\n";
        if (kind == Kind::kPoll) {
            poll_started_ = Now();
            app_.log().Info("NETEASE_LOGIN poll start: receive_timeout_ms=35000");
        }
        headers += "Cache-Control: no-cache\r\n";
        if (!http_.Start(TransportUrl(current_url_), headers, body, !body.empty(), kind == Kind::kPoll ? 35000 : 10000))
            Fail("无法开始登录请求");
    }
    void VerifyRequest() {
        phase = LoginPhase::kLoading;
        message = "正在验证网易云账号…";
        auto data = "{\"csrf_token\":" + Quote(account_.Csrf()) + ",\"e_r\":false}";
        Request("https://music.163.com/weapi/w/nuser/account/get", Kind::kVerify, true, Weapi(data, app_.random()));
    }
    void Fail(std::string text) {
        http_.Cancel();
        kind_ = Kind::kNone;
        phase = LoginPhase::kError;
        message = std::move(text);
    }
    micropixel::Application& app_;
    Account& account_;
    WebRequest http_;
    Kind kind_{Kind::kNone};
    std::string current_url_, callback_, state_, uuid_, fixture_, poll_origin_;
    uint64_t deadline_{}, next_poll_{}, poll_started_{};
    unsigned redirects_{}, last_{};
};
}  // namespace netease
