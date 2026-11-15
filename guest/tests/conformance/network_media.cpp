// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "abi/micropixel_abi.h"
#include "apps/netease-music/json.hpp"
#include "apps/netease-music/weapi.hpp"
#include "sdk/micropixel.hpp"
using namespace micropixel;
using namespace micropixel::literals;
int main() {
    Application app;
    const std::array<uint8_t, 16> key{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p'};
    Assert(netease::AesBase64("Hello MicroPixel", key) == "Y5LZAFhg5lYP4RX7plH4OL/6K5AOjFtiopRuU57nOik=",
           "AES known vector");
    Assert(netease::RsaSecret(key) ==
               "d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59d53b32358903a121b5fca784c1f5ae44951fd0"
               "2524df58ecc98e52cc7cf8689b42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13ef4cafbb0cf5a6"
               "6731a3f1776c75ca35d0062dac70a3e33245afabcf47938487",
           "RSA known vector");
    netease::Json json("{\"a\":[{\"n\":101,\"s\":\"\\u4e2d\\ud83c\\udfb5\"}],\"code\":-460}");
    Assert(json.valid() && json.Number(json.Field(0, "code")) == -460, "JSON signed number");
    netease::Json flags("{\"more\":true,\"none\":false}");
    Assert(flags.Boolean(flags.Field(0, "more")) && !flags.Boolean(flags.Field(0, "none")), "JSON pagination flags");
    auto array = json.Field(0, "a");
    Assert(json.Count(array) == 1, "JSON array");
    Assert(json.String(json.Field(json.At(array, 0), "s")) == "中🎵", "JSON Unicode");
    for (auto invalid : {"[1,]", "{\"a\":1}junk", "\"\\ud800\"", "\"\\udc00\"", "01", "[true false]"})
        Assert(!netease::Json(invalid).valid(), "malformed JSON accepted");
    std::string deep(26, '[');
    deep += '0';
    deep += std::string(26, ']');
    Assert(!netease::Json(deep).valid(), "JSON depth");
    const char* origin = app.launch_arguments().FindValue("--test-origin");
    Assert(origin, "test origin missing");
    std::string base = origin;
    micropixel_service_info_t service{};
    Assert(micropixel_service_open(MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_INTERFACE_VERSION(1, 0), &service,
                                   sizeof(service)) == 0,
           "network service open");
    auto raw = [&](uint32_t method, const void* req, uint32_t size, void* response, uint32_t cap) {
        uint32_t out = 0;
        return micropixel_service_call(service.service_handle, method, static_cast<const uint8_t*>(req), size,
                                       static_cast<uint8_t*>(response), cap, &out);
    };
    std::string url = base + "/probe";
    std::vector<uint8_t> request(sizeof(micropixel_http_start_request_t) + url.size());
    micropixel_http_start_request_t start{sizeof(start), MICROPIXEL_HTTP_GET, static_cast<uint32_t>(url.size()), 0, 0,
                                          10000};
    __builtin_memcpy(request.data(), &start, sizeof(start));
    __builtin_memcpy(request.data() + sizeof(start), url.data(), url.size());
    micropixel_handle_response_t handle{};
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_START, request.data(), request.size() - 1, &handle, sizeof(handle)) ==
               MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "truncated HTTP payload");
    auto bad = start;
    bad.url_length = UINT32_MAX;
    __builtin_memcpy(request.data(), &bad, sizeof(bad));
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_START, request.data(), request.size(), &handle, sizeof(handle)) ==
               MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "HTTP length overflow");
    __builtin_memcpy(request.data(), &start, sizeof(start));
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_START, request.data(), request.size(), &handle, sizeof(handle) - 1) ==
               MICROPIXEL_STATUS_BUFFER_TOO_SMALL,
           "HTTP short response");
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_START, request.data(), request.size(), &handle, sizeof(handle)) == 0,
           "HTTP start after short response");
    micropixel_handle_response_t duplicate{};
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_START, request.data(), request.size(), &duplicate, sizeof(duplicate)) ==
               MICROPIXEL_STATUS_RESOURCE_EXHAUSTED,
           "HTTP capacity");
    micropixel_handle_request_t close{sizeof(close), 0, handle.handle + 1};
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE, &close, sizeof(close), nullptr, 0) ==
               MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "foreign HTTP handle");
    close.handle = handle.handle;
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE, &close, sizeof(close), nullptr, 0) == 0, "HTTP close");
    Assert(raw(MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE, &close, sizeof(close), nullptr, 0) ==
               MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "stale HTTP handle");
    auto requested = app.network().Start(HttpMethod::kGet, url);
    Assert(requested.has_value(), "SDK HTTP start");
    auto http = std::move(*requested);
    auto invalid_media = app.audio().OpenUrl("file:///etc/passwd");
    Assert(!invalid_media, "local path accepted as URL media");
    MediaPlayback media;
    bool overflow = false, paused = false;
    unsigned ticks = 0;
    auto timer = app.timers().Every(20_ms).value();
    app.Run([&](const Event& event) -> EventResult {
        if (event.type() == EventType::kStop) return EventResult::kExit;
        if (!event.TimerFrom(timer)) return EventResult::kContinue;
        Assert(++ticks < 500, "remote service timeout");
        if (http.valid()) {
            auto state = http.State();
            Assert(state.has_value(), "HTTP state");
            if (state->phase == HttpPhase::kPending) return EventResult::kContinue;
            if (!overflow) {
                Assert(state->phase == HttpPhase::kComplete && state->body_length == 6000, "HTTP body length");
                std::array<uint8_t, 4096> bytes{};
                auto first = http.Read(0, bytes);
                Assert(first && *first == 4096, "HTTP first chunk");
                for (unsigned i = 0; i < 4096; ++i) Assert(bytes[i] == '0' + i % 10, "HTTP body data");
                auto second = http.Read(4096, bytes);
                Assert(second && *second == 1904, "HTTP tail chunk");
                Assert(!http.Read(6001, bytes), "HTTP read offset overflow");
                http.Close();
                auto small = app.network().Start(HttpMethod::kGet, url, {}, {}, 4);
                Assert(small.has_value(), "HTTP bounded response start");
                http = std::move(*small);
                overflow = true;
            } else {
                Assert(state->phase == HttpPhase::kFailed, "HTTP response limit ignored");
                http.Close();
                auto opened = app.audio().OpenUrl(base + "/tone.mp3");
                Assert(opened.has_value(), "URL media open");
                media = std::move(*opened);
                Assert(!app.audio().OpenUrl(base + "/tone.mp3"), "media capacity");
            }
        } else if (media.valid()) {
            auto state = media.State();
            Assert(state.has_value(), "URL media state");
            Assert(state->phase != MediaPhase::kFailed, "URL media decode");
            if (state->phase == MediaPhase::kLoading) return EventResult::kContinue;
            if (!paused) {
                Assert(state->duration_ms > 1000, "URL media duration");
                Assert(media.Pause(true).has_value(), "URL media pause");
                Assert(media.Seek(1000).has_value(), "URL media seek");
                paused = true;
            } else {
                auto check = media.State();
                Assert(check && check->phase == MediaPhase::kPaused && check->position_ms == 1000,
                       "URL media paused seek state");
                Assert(media.Pause(false).has_value(), "URL media resume");
                media.Close();
                app.log().Info("NETWORK_MEDIA PASS: crypto JSON bounds chunks handles cancellation playback");
                return EventResult::kExit;
            }
        }
        return EventResult::kContinue;
    });
    return 0;
}
