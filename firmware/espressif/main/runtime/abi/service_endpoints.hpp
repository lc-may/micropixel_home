#ifndef MICROPIXEL_RUNTIME_ABI_SERVICE_ENDPOINTS_HPP
#define MICROPIXEL_RUNTIME_ABI_SERVICE_ENDPOINTS_HPP

#include <array>
#include <cstring>
#include <string_view>

#include "device/contracts/remote.hpp"
#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {

// PCM writes carry samples after the fixed header. Copy the header so the wire
// buffer need not satisfy the alignment of the native request struct.
inline bool ReadPcmStreamWriteRequest(const uint8_t* request, uint32_t request_size,
                                      micropixel_audio_pcm_stream_write_request_t& wire) {
    if (request == nullptr || request_size < sizeof(wire)) {
        return false;
    }
    std::memcpy(&wire, request, sizeof(wire));
    return wire.size == request_size && wire.reserved0 == 0U;
}

class GuestContext;

class DevicesServiceEndpoint final : public ServiceHandler {
   public:
    explicit DevicesServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class SensorsServiceEndpoint final : public ServiceHandler {
   public:
    explicit SensorsServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class GpioServiceEndpoint final : public ServiceHandler {
   public:
    explicit GpioServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class HapticsServiceEndpoint final : public ServiceHandler {
   public:
    explicit HapticsServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class PowerInfoServiceEndpoint final : public ServiceHandler {
   public:
    explicit PowerInfoServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class SystemServiceEndpoint final : public ServiceHandler {
   public:
    SystemServiceEndpoint(std::string_view effective_locale,
                          const micropixel_system_launch_arguments_response_t& launch_arguments);
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    std::array<char, MICROPIXEL_LOCALE_TAG_MAX_BYTES + 1U> effective_locale_{};
    uint16_t effective_locale_length_{};
    micropixel_system_launch_arguments_response_t launch_arguments_{};
};

class TimerServiceEndpoint final : public ServiceHandler {
   public:
    explicit TimerServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class StorageServiceEndpoint final : public ServiceHandler {
   public:
    explicit StorageServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class ResourceServiceEndpoint final : public ServiceHandler {
   public:
    explicit ResourceServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class RandomServiceEndpoint final : public ServiceHandler {
   public:
    explicit RandomServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class GraphicsServiceEndpoint final : public ServiceHandler {
   public:
    explicit GraphicsServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;
    [[nodiscard]] int32_t Submit(uint32_t channel_id, const uint8_t* bytes, uint32_t length) override;

   private:
    GuestContext& context_;
};

class InputServiceEndpoint final : public ServiceHandler {
   public:
    explicit InputServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class AudioServiceEndpoint final : public ServiceHandler {
   public:
    explicit AudioServiceEndpoint(GuestContext& context) : context_(context) {}
    [[nodiscard]] ServiceDescriptor Describe() const override;
    [[nodiscard]] int32_t Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                               uint32_t response_capacity, uint32_t& response_size_out) override;

   private:
    GuestContext& context_;
};

class NetworkServiceEndpoint final : public ServiceHandler {
   public:
    explicit NetworkServiceEndpoint(device::HttpClient* backend) : backend_(backend) {}
    ~NetworkServiceEndpoint() override {
        if (handle_ && backend_) (void)backend_->Close(handle_);
    }
    ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&) override;

   private:
    device::HttpClient* backend_{};
    uint32_t handle_{};
};
class UrlMediaEndpoint final {
   public:
    explicit UrlMediaEndpoint(device::UrlMedia* backend) : backend_(backend) {}
    ~UrlMediaEndpoint() { Close(); }
    bool Available() const { return backend_ != nullptr; }
    void Close() {
        if (handle_ && backend_) (void)backend_->Close(handle_);
        handle_ = 0;
    }
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&);

   private:
    device::UrlMedia* backend_{};
    uint32_t handle_{};
};

}  // namespace micropixel::runtime

#endif
