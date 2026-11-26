// SPDX-License-Identifier: Apache-2.0
#include "media.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "bflb_mtimer.h"
#include "device/contracts/remote.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mm.h"
#include "mp3_stream.hpp"
#include "platform/audio/audio_engine.hpp"
#include "platform/network/https_get.h"
#include "platform/network/network_time.h"
#include "platform/wireless/wireless.h"
#include "pvmp3dec/pvmp3_framedecoder.h"
#include "work/background_task.hpp"
#include "work/task_policy.hpp"

namespace micropixel::bouffalo {
namespace {

constexpr uint32_t kMediaTaskStackBytes = 16U * 1024U;
BackgroundTask media_worker;

constexpr uint32_t kMaxStreamBytes = 64U * 1024U * 1024U;
constexpr uint32_t kRingFrames = 8192;
constexpr unsigned kDecodeWarmupFrames = 10;

struct PsramFree {
    void operator()(void* memory) const { kfree(memory); }
};

struct Workspace final {
    char worker_url[MICROPIXEL_MEDIA_MAX_URL_BYTES + 1];
    Mp3Stream stream;
    tPVMP3DecoderExternal decoder_config;
    alignas(4) uint8_t input[Mp3Stream::kBufferSize];
    int16_t output[2304];
    int16_t ring[kRingFrames];
};

class Lock final {
   public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    SemaphoreHandle_t mutex_;
};

class MusicOutput final : public device::UrlMedia {
   public:
    MusicOutput(platform::audio::AudioEngine& audio, uint32_t sample_rate)
        : audio_(audio),
          sample_rate_(sample_rate),
          prefill_(std::clamp<uint32_t>(sample_rate / 5U, 1U, kRingFrames / 2U)) {}
    bool Initialize() {
        control_ = xSemaphoreCreateMutexStatic(&control_storage_);
        if (!control_) return false;
        if (media_worker.Start(Worker, "mpx_music", kMediaTaskStackBytes, this, task_policy::kRemoteControlPriority))
            return true;
        vSemaphoreDelete(control_);
        control_ = nullptr;
        return false;
    }

    int32_t Open(std::string_view url, uint32_t& handle) override {
        if (!url.starts_with("https://") || url.size() > MICROPIXEL_MEDIA_MAX_URL_BYTES || url.find('\0') != url.npos ||
            std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!mpx_wifi_online() || !mpx_network_time_ready()) return MICROPIXEL_STATUS_TIMEOUT;
        Lock lock(control_);
        if (handle_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        std::memcpy(url_, url.data(), url.size());
        url_[url.size()] = '\0';
        const mpx_https_request_t request{.url = url_};
        if (!mpx_https_valid_request(&request)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (++next_handle_ == 0) ++next_handle_;
        handle = handle_ = next_handle_;
        paused_.store(false, std::memory_order_relaxed);
        state_ = {.size = sizeof(state_), .state = MICROPIXEL_MEDIA_LOADING};
        seek_position_ms_ = 0;
        seek_serial_.store(0, std::memory_order_relaxed);
        desired_.store(handle_, std::memory_order_release);
        xTaskNotifyGive(media_worker.Handle());
        return MICROPIXEL_STATUS_OK;
    }

    int32_t State(uint32_t handle, micropixel_media_state_t& state) override {
        Lock lock(control_);
        if (!handle || handle != handle_) return MICROPIXEL_STATUS_STALE_STATE;
        state = state_;
        if (state.state == MICROPIXEL_MEDIA_PLAYING && playing_.load(std::memory_order_acquire) == handle) {
            const auto consumed = read_.load(std::memory_order_relaxed);
            state.position_ms = std::min<uint64_t>(UINT32_MAX, state_.position_ms + consumed * 1000ULL / sample_rate_);
            if (state.duration_ms) state.duration_ms = std::max(state.duration_ms, state.position_ms);
        }
        if (state.state == MICROPIXEL_MEDIA_PLAYING && paused_.load(std::memory_order_relaxed))
            state.state = MICROPIXEL_MEDIA_PAUSED;
        return MICROPIXEL_STATUS_OK;
    }

    int32_t Pause(uint32_t handle, bool paused) override {
        Lock lock(control_);
        if (!handle || handle != handle_) return MICROPIXEL_STATUS_STALE_STATE;
        paused_.store(paused, std::memory_order_relaxed);
        return MICROPIXEL_STATUS_OK;
    }

    int32_t Seek(uint32_t handle, uint32_t position_ms) override {
        Lock lock(control_);
        if (!handle || handle != handle_) return MICROPIXEL_STATUS_STALE_STATE;
        if (state_.state != MICROPIXEL_MEDIA_PLAYING &&
            (state_.state != MICROPIXEL_MEDIA_LOADING || !state_.duration_ms))
            return MICROPIXEL_STATUS_WOULD_BLOCK;
        seek_position_ms_ = state_.duration_ms ? std::min(position_ms, state_.duration_ms) : position_ms;
        state_.position_ms = seek_position_ms_;
        state_.state = MICROPIXEL_MEDIA_LOADING;
        seek_serial_.fetch_add(1, std::memory_order_release);
        return MICROPIXEL_STATUS_OK;
    }

    int32_t Close(uint32_t handle) override {
        Lock lock(control_);
        if (!handle || handle != handle_) return MICROPIXEL_STATUS_STALE_STATE;
        desired_.store(0, std::memory_order_release);
        handle_ = 0;
        return MICROPIXEL_STATUS_OK;
    }

   private:
    static void Worker(void* raw) { static_cast<MusicOutput*>(raw)->Run(); }

    void Run() {
        for (;;) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            uint32_t handle;
            {
                Lock lock(control_);
                handle = handle_;
                if (!handle) continue;
                std::strcpy(workspace_.worker_url, url_);
            }
            job_ = handle;
            Load();
            StopVoice();
        }
    }

    bool Active() const { return desired_.load(std::memory_order_acquire) == job_; }

    bool Interrupted() const { return !Active() || seek_serial_.load(std::memory_order_acquire) != job_seek_serial_; }

    static bool CancelTransfer(void* raw) {
        return static_cast<MusicOutput*>(raw)->Interrupted() || !mpx_wifi_online();
    }

    static bool BeginBody(void* raw, uint16_t status, uint64_t length) {
        auto& self = *static_cast<MusicOutput*>(raw);
        if (self.Interrupted()) return false;
        if (status != 200) {
            self.error_ = "HTTPS audio request failed";
            return false;
        }
        if (length != UINT64_MAX && length > kMaxStreamBytes) {
            self.error_ = "MP3 response too large";
            return false;
        }
        self.content_length_ = length == UINT64_MAX ? 0 : static_cast<uint32_t>(length);
        return true;
    }

    static bool ReceiveBody(void* raw, const uint8_t* bytes, size_t length) {
        auto& self = *static_cast<MusicOutput*>(raw);
        if (self.Interrupted()) return false;
        self.received_ += length;
        return self.workspace_.stream.Push({bytes, length}, [&self](auto frame, const Mp3Frame& header, uint32_t n) {
            return self.HandleFrame(frame, header, n);
        });
    }

    bool Publish(uint16_t phase, uint32_t duration, const char* error = "") {
        Lock lock(control_);
        if (Interrupted()) return false;
        state_.state = phase;
        state_.duration_ms = duration;
        if (phase == MICROPIXEL_MEDIA_FINISHED) state_.position_ms = duration;
        std::snprintf(state_.message, sizeof(state_.message), "%s", error);
        return true;
    }

    void Load() {
        std::unique_ptr<void, PsramFree> decoder(
            kmalloc(pvmp3_decoderMemRequirements(), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
        while (Active()) {
            StopVoice();
            uint32_t target;
            {
                Lock lock(control_);
                if (handle_ != job_) break;
                job_seek_serial_ = seek_serial_.load(std::memory_order_relaxed);
                target = seek_position_ms_;
            }
            if (!decoder) {
                Publish(MICROPIXEL_MEDIA_FAILED, 0, "Not enough memory for MP3 decoder");
                break;
            }
            ResetStream(decoder.get());
            Stream(target);
            if (!Interrupted()) break;
        }
    }

    void ResetStream(void* decoder) {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        end_.store(false, std::memory_order_relaxed);
        buffering_ = false;
        resample_phase_ = average_count_ = 0;
        average_ = 0;
        received_ = content_length_ = duration_ = target_frame_ = bad_ = 0;
        error_ = nullptr;
        decoder_ = decoder;
        workspace_.stream.Reset();
        workspace_.decoder_config = {};
        pvmp3_InitDecoder(&workspace_.decoder_config, decoder_);
        started_ms_ = bflb_mtimer_get_time_ms();
    }

    void Stream(uint32_t target) {
        target_ms_ = target;
        const mpx_https_request_t request{.url = workspace_.worker_url, .receive_timeout_ms = 15000};
        mpx_https_response_t response{.context = this,
                                      .cancelled = CancelTransfer,
                                      .capacity = kMaxStreamBytes,
                                      .on_headers = BeginBody,
                                      .on_body = ReceiveBody};
        const int result = mpx_https_exchange(&request, &response);
        if (Interrupted()) return;
        if (result || response.status != 200 || !workspace_.stream.Complete()) {
            const char* error = error_                                ? error_
                                : response.overflow                   ? "MP3 response too large"
                                : !mpx_wifi_online()                  ? "Network disconnected"
                                : response.status == 200 || received_ ? "Incomplete or invalid MP3 data"
                                                                      : "HTTPS audio download failed";
            Publish(MICROPIXEL_MEDIA_FAILED, duration_, error);
            return;
        }
        duration_ = workspace_.stream.Duration();
        end_.store(true, std::memory_order_release);
        if (Queued() && !voice_ && !StartVoice()) return;
        if (!voice_ && target_ms_ < duration_) {
            Publish(MICROPIXEL_MEDIA_FAILED, duration_, "MP3 contains no playable frames");
            return;
        }
        if (voice_) Publish(MICROPIXEL_MEDIA_PLAYING, duration_);
        while (!Interrupted() && Queued()) vTaskDelay(pdMS_TO_TICKS(10));
        if (Publish(MICROPIXEL_MEDIA_FINISHED, duration_))
            std::printf("MicroPixel MP3 complete (%lu ms, %lu bytes)\n", static_cast<unsigned long>(duration_),
                        static_cast<unsigned long>(received_));
    }

    void StopVoice() {
        if (!voice_) return;
        while (audio_.StopPcm(voice_) == MICROPIXEL_STATUS_RATE_LIMITED) vTaskDelay(pdMS_TO_TICKS(10));
        voice_ = 0;
        playing_.store(0, std::memory_order_release);
    }

    uint32_t Queued() const { return write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire); }

    bool StartVoice() {
        if (Interrupted()) return false;
        if (audio_.StartPcm({ReadPcm, this}, 0, 1000, voice_) != MICROPIXEL_STATUS_OK) {
            error_ = "Audio output busy";
            Publish(MICROPIXEL_MEDIA_FAILED, duration_, error_);
            return false;
        }
        playing_.store(job_, std::memory_order_release);
        if (!Publish(MICROPIXEL_MEDIA_PLAYING, duration_)) return false;
        std::printf("MicroPixel MP3 playing (%lu ms, %lu bytes received, %lu ms since request)\n",
                    static_cast<unsigned long>(duration_), static_cast<unsigned long>(received_),
                    static_cast<unsigned long>(bflb_mtimer_get_time_ms() - started_ms_));
        return true;
    }

    static device::PcmReadResult ReadPcm(void* raw, int16_t* destination, uint32_t capacity) {
        auto& self = *static_cast<MusicOutput*>(raw);
        if (self.Interrupted()) return {0, true};
        if (self.paused_.load(std::memory_order_relaxed)) return {};
        const auto read = self.read_.load(std::memory_order_relaxed);
        const bool end = self.end_.load(std::memory_order_acquire);
        const auto queued = self.write_.load(std::memory_order_acquire) - read;
        if (!end && self.buffering_ && queued < self.prefill_) return {};
        const auto count = std::min(capacity, queued);
        for (uint32_t i = 0; i < count; ++i) destination[i] = self.workspace_.ring[(read + i) % kRingFrames];
        self.buffering_ = count < capacity;
        self.read_.store(read + count, std::memory_order_release);
        return {count, end && count == queued};
    }

    uint32_t EstimateDuration(std::span<const uint8_t> bytes, const Mp3Frame& frame) const {
        const auto tagged_frames = Mp3TaggedFrameCount(bytes, frame);
        if (tagged_frames) return frame.Duration(tagged_frames);
        const auto prefix = workspace_.stream.PrefixBytes();
        return content_length_ > prefix ? (content_length_ - prefix) * 8ULL / frame.bitrate_kbps : 0;
    }

    bool HandleFrame(std::span<const uint8_t> bytes, const Mp3Frame& frame, uint32_t number) {
        if (Interrupted()) return false;
        if (!number) {
            duration_ = EstimateDuration(bytes, frame);
            target_frame_ = target_ms_ * uint64_t(frame.rate) / (1000U * frame.samples);
            Lock lock(control_);
            if (Interrupted()) return false;
            state_.position_ms = frame.Duration(target_frame_);
            if (duration_) state_.duration_ms = duration_;
        }
        if (number + kDecodeWarmupFrames < target_frame_) return true;
        const bool output = number >= target_frame_;
        if (output) {
            const uint32_t needed = (frame.samples * sample_rate_ + frame.rate - 1) / frame.rate;
            while (Queued() + needed > kRingFrames) {
                if (CancelTransfer(this)) return false;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (DecodeFrame(bytes, frame, output)) {
            bad_ = 0;
        } else if (++bad_ >= 8) {
            error_ = "Invalid MP3 data";
            return false;
        }
        return voice_ || Queued() < prefill_ || StartVoice();
    }

    bool DecodeFrame(std::span<const uint8_t> input, const Mp3Frame& frame, bool enqueue) {
        if (input.size() + 4 > sizeof(workspace_.input)) return false;
        std::memcpy(workspace_.input, input.data(), input.size());
        std::memset(workspace_.input + input.size(), 0, 4);
        auto& config = workspace_.decoder_config;
        config.pInputBuffer = workspace_.input;
        config.inputBufferCurrentLength = input.size();
        config.inputBufferMaxLength = sizeof(workspace_.input);
        config.inputBufferUsedLength = 0;
        config.pOutputBuffer = workspace_.output;
        config.outputFrameSize = std::size(workspace_.output);
        const auto decoded = pvmp3_framedecoder(&config, decoder_);
        if (decoded == NO_ENOUGH_MAIN_DATA_ERROR) return true;
        if (decoded != NO_DECODING_ERROR || config.samplingRate != static_cast<int32_t>(frame.rate) ||
            (config.num_channels != 1 && config.num_channels != 2) || config.outputFrameSize <= 0 ||
            static_cast<size_t>(config.outputFrameSize) > std::size(workspace_.output) ||
            config.outputFrameSize % config.num_channels ||
            uint32_t(config.outputFrameSize / config.num_channels) > frame.samples)
            return false;
        if (!enqueue) return true;
        auto write = write_.load(std::memory_order_relaxed);
        for (int32_t i = 0; i < config.outputFrameSize; i += config.num_channels) {
            const int32_t left = workspace_.output[i];
            const int32_t mono = config.num_channels == 1 ? left : (left + workspace_.output[i + 1]) / 2;
            average_ += mono;
            ++average_count_;
            resample_phase_ += sample_rate_;
            if (resample_phase_ < frame.rate) continue;
            const int16_t sample = average_ / static_cast<int32_t>(average_count_);
            do {
                workspace_.ring[write++ % kRingFrames] = sample;
                resample_phase_ -= frame.rate;
            } while (resample_phase_ >= frame.rate);
            average_ = 0;
            average_count_ = 0;
        }
        write_.store(write, std::memory_order_release);
        return true;
    }

    platform::audio::AudioEngine& audio_;
    const uint32_t sample_rate_, prefill_;
    StaticSemaphore_t control_storage_{};
    SemaphoreHandle_t control_{};
    Workspace workspace_;
    char url_[MICROPIXEL_MEDIA_MAX_URL_BYTES + 1];
    micropixel_media_state_t state_{};
    std::atomic<uint32_t> desired_{}, playing_{}, read_{}, write_{}, seek_serial_{};
    std::atomic<bool> paused_{}, end_{};
    device::PcmStreamHandle voice_{};
    uint32_t handle_{}, next_handle_{}, job_{}, job_seek_serial_{}, seek_position_ms_{};
    uint32_t received_{}, content_length_{}, duration_{}, target_ms_{}, target_frame_{}, bad_{};
    uint64_t started_ms_{};
    const char* error_{};
    void* decoder_{};
    bool buffering_{};
    uint32_t resample_phase_{}, average_count_{};
    int32_t average_{};
};

MusicOutput* media;

}  // namespace

bool InitializeMedia(platform::audio::AudioEngine& audio, uint32_t sample_rate) {
    auto* memory = static_cast<MusicOutput*>(kmalloc(sizeof(MusicOutput), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (!memory) return false;
    media = std::construct_at(memory, audio, sample_rate);
    if (media->Initialize()) return true;
    std::destroy_at(media);
    kfree(media);
    media = nullptr;
    return false;
}

device::UrlMedia* MediaBackend() { return media; }

}  // namespace micropixel::bouffalo
