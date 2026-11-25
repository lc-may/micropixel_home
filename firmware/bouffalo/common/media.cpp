// SPDX-License-Identifier: Apache-2.0
#include "media.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "background_task.hpp"
#include "device/contracts/remote.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "https_get.h"
#include "network_time.h"
#include "platform/audio/audio_engine.hpp"
#include "pvmp3dec/pvmp3_framedecoder.h"
#include "wireless.h"
#include "work/task_policy.hpp"

namespace micropixel::bouffalo {
namespace {

constexpr uint32_t kMediaTaskStackBytes = 16U * 1024U;
BackgroundTask media_worker;

constexpr uint32_t kMaxTrack = 4 * 1024 * 1024;
constexpr uint32_t kRingFrames = 8192;
constexpr uint32_t kFrameInput = 2048;
constexpr unsigned kDecodeWarmupFrames = 10;

struct PsramFree {
    void operator()(void* memory) const { heap_caps_free(memory); }
};

struct Frame final {
    uint32_t bytes{}, rate{}, samples{};
};

Frame ParseFrame(const uint8_t* p) {
    // MPEG-1/2/2.5 Layer III. The header determines the exact byte length,
    // so the SDK decoder only sees complete frames in its private input buffer.
    const unsigned version = (p[1] >> 3) & 3;
    const unsigned bitrate = p[2] >> 4;
    const unsigned rate_index = (p[2] >> 2) & 3;
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0 || (p[1] & 6) != 2 || version == 1 || !bitrate || bitrate == 15 ||
        rate_index == 3)
        return {};
    constexpr uint16_t kMpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr uint16_t kMpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    constexpr uint32_t kRates[] = {44100, 48000, 32000};
    const uint32_t rate = kRates[rate_index] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    const uint32_t length = (version == 3 ? 144000 * kMpeg1[bitrate] : 72000 * kMpeg2[bitrate]) / rate;
    return {length + ((p[2] >> 1) & 1), rate, version == 3 ? 1152U : 576U};
}

struct Track final {
    size_t begin{}, end{};
    uint32_t rate{}, samples{}, frames{};
    uint32_t Duration() const { return static_cast<uint64_t>(frames) * samples * 1000 / rate; }
};

Track IndexTrack(std::span<const uint8_t> bytes) {
    size_t start = 0;
    if (bytes.size() >= 10 && std::memcmp(bytes.data(), "ID3", 3) == 0) {
        for (unsigned i = 6; i < 10; ++i) {
            if (bytes[i] & 0x80) return {};
            start = (start << 7) | bytes[i];
        }
        start += bytes[3] == 4 && (bytes[5] & 0x10) ? 20 : 10;
    }
    if (start >= bytes.size()) return {};
    // Permit alignment/padding before the first frame and trailing metadata.
    for (const size_t limit = std::min(bytes.size(), start + 4096); start + 4 <= limit; ++start) {
        const auto header = ParseFrame(bytes.data() + start);
        if (!header.bytes || header.bytes > bytes.size() - start) continue;
        Track track{.begin = start, .end = start, .rate = header.rate, .samples = header.samples};
        while (track.end + 4 <= bytes.size()) {
            const auto frame = ParseFrame(bytes.data() + track.end);
            if (!frame.bytes || frame.rate != track.rate || frame.samples != track.samples ||
                frame.bytes > bytes.size() - track.end)
                break;
            track.end += frame.bytes;
            ++track.frames;
        }
        if (track.frames >= 2) return track;
    }
    return {};
}

struct Workspace final {
    char worker_url[MICROPIXEL_MEDIA_MAX_URL_BYTES + 1];
    char response_headers[MICROPIXEL_HTTP_MAX_HEADER_BYTES];
    alignas(4) uint8_t input[kFrameInput];
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
    MusicOutput(platform::audio::AudioEngine& audio, uint32_t sample_rate) : audio_(audio), sample_rate_(sample_rate) {}
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
        desired_.store(handle_, std::memory_order_release);
        paused_.store(false, std::memory_order_relaxed);
        state_ = {.size = sizeof(state_), .state = MICROPIXEL_MEDIA_LOADING};
        seek_serial_ = 0;
        xTaskNotifyGive(media_worker.Handle());
        return MICROPIXEL_STATUS_OK;
    }

    int32_t State(uint32_t handle, micropixel_media_state_t& state) override {
        Lock lock(control_);
        if (!handle || handle != handle_) return MICROPIXEL_STATUS_STALE_STATE;
        state = state_;
        if (playing_.load(std::memory_order_acquire) == handle) {
            const auto consumed = read_.load(std::memory_order_relaxed);
            state.position_ms =
                std::min<uint64_t>(state.duration_ms, seek_base_ms_ + consumed * 1000ULL / sample_rate_);
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
        if (state_.state != MICROPIXEL_MEDIA_PLAYING) return MICROPIXEL_STATUS_WOULD_BLOCK;
        seek_position_ms_ = std::min(position_ms, state_.duration_ms);
        ++seek_serial_;
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
    static bool CancelTransfer(void* raw) { return !static_cast<MusicOutput*>(raw)->Active() || !mpx_wifi_online(); }

    void Publish(uint16_t phase, uint32_t duration, const char* error = "") {
        Lock lock(control_);
        if (handle_ != job_) return;
        state_.state = phase;
        state_.duration_ms = duration;
        if (phase == MICROPIXEL_MEDIA_FINISHED) state_.position_ms = duration;
        std::snprintf(state_.message, sizeof(state_.message), "%s", error);
    }

    void Load() {
        std::unique_ptr<uint8_t, PsramFree> encoded(
            static_cast<uint8_t*>(heap_caps_malloc(kMaxTrack, MALLOC_CAP_SPIRAM)));
        if (!encoded) {
            Publish(MICROPIXEL_MEDIA_FAILED, 0, "Not enough memory for MP3");
            return;
        }
        const mpx_https_request_t request{.url = workspace_.worker_url, .receive_timeout_ms = 15000};
        mpx_https_response_t response{.context = this,
                                      .cancelled = CancelTransfer,
                                      .body = encoded.get(),
                                      .capacity = kMaxTrack,
                                      .headers = workspace_.response_headers,
                                      .headers_capacity = sizeof(workspace_.response_headers)};
        if (mpx_https_exchange(&request, &response) || response.status != 200) {
            Publish(MICROPIXEL_MEDIA_FAILED, 0,
                    response.overflow ? "MP3 exceeds 4 MiB" : "HTTPS audio download failed");
            return;
        }
        if (!Active()) return;
        const auto track = IndexTrack({encoded.get(), response.length});
        if (!track.frames) {
            Publish(MICROPIXEL_MEDIA_FAILED, 0, "Unsupported audio format");
            return;
        }
        std::unique_ptr<void, PsramFree> decoder(heap_caps_malloc(pvmp3_decoderMemRequirements(), MALLOC_CAP_SPIRAM));
        if (!decoder) {
            Publish(MICROPIXEL_MEDIA_FAILED, track.Duration(), "Not enough memory for MP3 decoder");
            return;
        }
        Decode(encoded.get(), track, decoder.get());
    }

    void StopVoice() {
        if (!voice_) return;
        while (audio_.StopPcm(voice_) == MICROPIXEL_STATUS_RATE_LIMITED) vTaskDelay(pdMS_TO_TICKS(10));
        voice_ = 0;
        playing_.store(0, std::memory_order_release);
    }

    static device::PcmReadResult ReadPcm(void* raw, int16_t* destination, uint32_t capacity) {
        auto& self = *static_cast<MusicOutput*>(raw);
        if (!self.Active()) return {0, true};
        if (self.paused_.load(std::memory_order_relaxed)) return {};
        const auto read = self.read_.load(std::memory_order_relaxed);
        const auto write = self.write_.load(std::memory_order_acquire);
        const auto count = std::min(capacity, write - read);
        for (uint32_t i = 0; i < count; ++i) destination[i] = self.workspace_.ring[(read + i) % kRingFrames];
        self.read_.store(read + count, std::memory_order_release);
        return {count, self.end_.load(std::memory_order_acquire) &&
                           read + count == self.write_.load(std::memory_order_acquire)};
    }

    bool DecodeFrame(const uint8_t* input, const Frame& frame, tPVMP3DecoderExternal& config, void* decoder,
                     bool enqueue) {
        if (frame.bytes + 4 > sizeof(workspace_.input)) return false;
        std::memcpy(workspace_.input, input, frame.bytes);
        std::memset(workspace_.input + frame.bytes, 0, 4);
        config.pInputBuffer = workspace_.input;
        config.inputBufferCurrentLength = frame.bytes;
        config.inputBufferMaxLength = sizeof(workspace_.input);
        config.inputBufferUsedLength = 0;
        config.pOutputBuffer = workspace_.output;
        config.outputFrameSize = std::size(workspace_.output);
        const auto decoded = pvmp3_framedecoder(&config, decoder);
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

    bool SeekChanged(uint32_t& revision, uint32_t& target) {
        Lock lock(control_);
        if (handle_ != job_ || seek_serial_ == revision) return false;
        revision = seek_serial_;
        target = seek_position_ms_;
        return true;
    }

    size_t ResetDecoder(const uint8_t* encoded, const Track& track, uint32_t target, tPVMP3DecoderExternal& config,
                        void* decoder) {
        StopVoice();
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        end_.store(false, std::memory_order_relaxed);
        resample_phase_ = average_count_ = 0;
        average_ = 0;
        config = {};
        pvmp3_InitDecoder(&config, decoder);
        const auto frame_number =
            std::min<uint64_t>(track.frames, target * uint64_t(track.rate) / (1000U * track.samples));
        size_t offset = track.begin;
        for (uint32_t i = 0; i < frame_number; ++i) {
            const auto frame = ParseFrame(encoded + offset);
            if (i + kDecodeWarmupFrames >= frame_number)
                (void)DecodeFrame(encoded + offset, frame, config, decoder, false);
            offset += frame.bytes;
        }
        {
            Lock lock(control_);
            seek_base_ms_ = frame_number * uint64_t(track.samples) * 1000 / track.rate;
        }
        return offset;
    }

    void Decode(const uint8_t* encoded, const Track& track, void* decoder) {
        tPVMP3DecoderExternal config{};
        uint32_t revision = 0, target = 0;
        auto offset = ResetDecoder(encoded, track, target, config, decoder);
        uint32_t bad = 0;
        const uint32_t worst_frame = (1152U * sample_rate_ + track.rate - 1) / track.rate + 1;
        while (Active()) {
            if (SeekChanged(revision, target)) {
                offset = ResetDecoder(encoded, track, target, config, decoder);
                bad = 0;
            }
            const uint32_t queued = write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire);
            if (offset == track.end) end_.store(true, std::memory_order_release);
            if (queued && !voice_ && (queued >= 1024 || offset == track.end)) {
                if (audio_.StartPcm({ReadPcm, this}, 0, 1000, voice_) != MICROPIXEL_STATUS_OK) {
                    Publish(MICROPIXEL_MEDIA_FAILED, track.Duration(), "Audio output busy");
                    return;
                }
                playing_.store(job_, std::memory_order_release);
                Publish(MICROPIXEL_MEDIA_PLAYING, track.Duration());
                std::printf("MicroPixel MP3 playing (%lu ms)\n", static_cast<unsigned long>(track.Duration()));
            }
            if (offset == track.end && !queued) {
                const bool finished = voice_ || target >= track.Duration();
                Publish(finished ? MICROPIXEL_MEDIA_FINISHED : MICROPIXEL_MEDIA_FAILED, track.Duration(),
                        finished ? "" : "MP3 contains no playable frames");
                return;
            }
            if (offset != track.end && queued + worst_frame <= kRingFrames) {
                const auto frame = ParseFrame(encoded + offset);
                if (DecodeFrame(encoded + offset, frame, config, decoder, true))
                    bad = 0;
                else if (++bad >= 8) {
                    Publish(MICROPIXEL_MEDIA_FAILED, track.Duration(), "Invalid MP3 data");
                    return;
                }
                offset += frame.bytes;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    platform::audio::AudioEngine& audio_;
    uint32_t sample_rate_;
    StaticSemaphore_t control_storage_{};
    SemaphoreHandle_t control_{};
    Workspace workspace_;
    char url_[MICROPIXEL_MEDIA_MAX_URL_BYTES + 1];
    micropixel_media_state_t state_{};
    std::atomic<uint32_t> desired_{}, playing_{}, read_{}, write_{};
    std::atomic<bool> paused_{}, end_{};
    device::PcmStreamHandle voice_{};
    uint32_t handle_{}, next_handle_{}, job_{}, seek_serial_{}, seek_position_ms_{}, seek_base_ms_{};
    uint32_t resample_phase_{}, average_count_{};
    int32_t average_{};
};

MusicOutput* media;

}  // namespace

bool InitializeMedia(platform::audio::AudioEngine& audio, uint32_t sample_rate) {
    auto* memory = static_cast<MusicOutput*>(heap_caps_malloc(sizeof(MusicOutput), MALLOC_CAP_SPIRAM));
    if (!memory) return false;
    media = std::construct_at(memory, audio, sample_rate);
    if (media->Initialize()) return true;
    std::destroy_at(media);
    heap_caps_free(media);
    media = nullptr;
    return false;
}

device::UrlMedia* MediaBackend() { return media; }

}  // namespace micropixel::bouffalo
