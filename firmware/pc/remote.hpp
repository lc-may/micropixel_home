// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "device/contracts/remote.hpp"
#include "media.hpp"
#include "network.hpp"
namespace micropixel::pc {
class GuestHttp final : public device::HttpClient {
   public:
    GuestHttp();
    ~GuestHttp() override;
    int32_t Start(uint16_t, std::string_view, std::string_view, std::span<const uint8_t>, uint32_t, uint32_t&) override;
    int32_t Exchange(uint16_t, std::string_view, std::string_view, std::span<const uint8_t>, uint32_t,
                     uint32_t&) override;
    int32_t TimedExchange(uint16_t, std::string_view, std::string_view, std::span<const uint8_t>, uint32_t, uint32_t,
                          uint32_t&) override;
    int32_t Metadata(uint32_t, micropixel_http_metadata_t&) override;
    int32_t Headers(uint32_t, uint32_t, std::span<uint8_t>, uint32_t&) override;
    int32_t State(uint32_t, micropixel_http_state_t&) override;
    int32_t Read(uint32_t, uint32_t, std::span<uint8_t>, uint32_t&) override;
    int32_t Close(uint32_t) override;

   private:
    int32_t Begin(bool, uint16_t, std::string_view, std::string_view, std::span<const uint8_t>, uint32_t, uint32_t&,
                  uint32_t receive_timeout_ms = 10000);
    struct Job {
        uint32_t handle{}, limit{}, receive_timeout_ms{10000};
        uint16_t method{};
        bool exchange{};
        std::string url, body;
        std::vector<HttpHeader> headers;
    };
    void Run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> pending_;
    uint32_t generation_{}, handle_{};
    bool shutdown_{};
    micropixel_http_state_t state_{};
    std::vector<uint8_t> body_;
    std::string headers_;
    micropixel_http_metadata_t metadata_{};
    std::atomic<bool> cancel_{true};
    std::thread worker_;
};
class GuestMedia final : public device::UrlMedia {
   public:
    explicit GuestMedia(MediaPlayer& player) : player_(player) {}
    int32_t Open(std::string_view, uint32_t&) override;
    int32_t State(uint32_t, micropixel_media_state_t&) override;
    int32_t Pause(uint32_t, bool) override;
    int32_t Seek(uint32_t, uint32_t) override;
    int32_t Close(uint32_t) override;

   private:
    MediaPlayer& player_;
    uint32_t handle_{}, generation_{};
};
}  // namespace micropixel::pc
