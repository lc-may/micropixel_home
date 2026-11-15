// SPDX-License-Identifier: Apache-2.0
#include "runtime/service_binding.hpp"
#include "sdk/messages.hpp"
#include "sdk/widgets.hpp"

namespace micropixel {
namespace {
runtime::ServiceCache widgets, messages;
uint32_t widget_revision{};
uint64_t sequence{};
int32_t Call(runtime::ServiceCache& cache, uint32_t service, uint32_t method, const void* request, uint32_t length,
             void* out, uint32_t capacity, uint32_t& written) {
    auto status = runtime::OpenService(cache, service, 1, 0);
    if (!status && !cache.info.capabilities) status = MICROPIXEL_STATUS_UNSUPPORTED;
    return status ? status : runtime::CallService(cache, method, request, length, out, capacity, written);
}
template <size_t N>
bool Text(char (&out)[N], std::string_view text) {
    if (text.size() >= N || text.find('\0') != std::string_view::npos) return false;
    if (!text.empty()) runtime::CopyBytes(out, text.data(), text.size());
    return true;
}
Result<uint32_t> Send(micropixel_message_send_t& wire, std::span<const uint8_t> payload) {
    if (payload.size() > sizeof(wire.payload) || !wire.type || wire.type > 31 || wire.ttl_ms < 100 ||
        wire.ttl_ms > 60000)
        return unexpected(Error{ErrorCode::kInvalidArgument});
    if (!wire.sequence && sequence == UINT64_MAX) return unexpected(Error{ErrorCode::kResourceExhausted});
    if (!wire.sequence)
        wire.sequence = ++sequence;
    else if (wire.sequence > sequence)
        sequence = wire.sequence;
    wire.size = sizeof(wire);
    wire.payload_length = payload.size();
    if (!payload.empty()) runtime::CopyBytes(wire.payload, payload.data(), payload.size());
    micropixel_handle_response_t result{};
    uint32_t written = 0;
    const auto status = Call(messages, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, &wire, sizeof(wire),
                             &result, sizeof(result), written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    if (written != sizeof(result) || result.size != sizeof(result) || !result.handle)
        return unexpected(Error{ErrorCode::kInternal});
    return result.handle;
}
}  // namespace
Result<uint32_t> Widgets::Publish(uint32_t id, const WidgetData& data) const {
    micropixel_widget_snapshot_t wire{};
    wire.size = sizeof(wire);
    wire.widget_id = id;
    wire.template_id = static_cast<uint16_t>(data.kind);
    if (widget_revision == UINT32_MAX) return unexpected(Error{ErrorCode::kResourceExhausted});
    wire.revision = ++widget_revision;
    wire.flags = (data.playing ? MICROPIXEL_WIDGET_PLAYING : 0) | (data.failed ? MICROPIXEL_WIDGET_ERROR : 0);
    wire.updated_utc = data.updated_utc;
    wire.valid_for_seconds = data.valid_for_seconds;
    wire.position_ms = data.position_ms;
    wire.duration_ms = data.duration_ms;
    wire.value = data.value;
    wire.auxiliary = data.auxiliary;
    if (!Text(wire.title, data.title) || !Text(wire.subtitle, data.subtitle))
        return unexpected(Error{ErrorCode::kInvalidArgument});
    micropixel_handle_response_t response{};
    uint32_t written = 0;
    const auto status = Call(widgets, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH, &wire, sizeof(wire),
                             &response, sizeof(response), written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    if (written != sizeof(response) || response.size != sizeof(response) || !response.handle)
        return unexpected(Error{ErrorCode::kInternal});
    return response.handle;
}
Result<void> Messages::Allow(std::string_view caller, uint32_t types) const {
    micropixel_message_allow_t wire{};
    wire.size = sizeof(wire);
    wire.types = types;
    if (!Text(wire.caller, caller)) return unexpected(Error{ErrorCode::kInvalidArgument});
    uint32_t written = 0;
    auto status =
        Call(messages, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_ALLOW, &wire, sizeof(wire), nullptr, 0, written);
    return status ? Result<void>(unexpected(runtime::ErrorFromStatus(status))) : Result<void>();
}
Result<uint32_t> Messages::Send(std::string_view app, uint16_t type, std::span<const uint8_t> payload, uint32_t ttl_ms,
                                uint64_t request_id) const {
    micropixel_message_send_t wire{};
    wire.type = type;
    wire.flags = MICROPIXEL_MESSAGE_LOGICAL;
    wire.ttl_ms = ttl_ms;
    wire.sequence = request_id;
    if (!Text(wire.target_app, app)) return unexpected(Error{ErrorCode::kInvalidArgument});
    return micropixel::Send(wire, payload);
}
Result<uint32_t> Messages::Send(InstanceAddress target, uint16_t type, std::span<const uint8_t> payload,
                                uint32_t ttl_ms, uint64_t request_id) const {
    micropixel_message_send_t wire{};
    wire.type = type;
    wire.target_boot = target.boot;
    wire.target_slot = target.slot;
    wire.target_epoch = target.epoch;
    wire.target_component = target.component;
    wire.ttl_ms = ttl_ms;
    wire.sequence = request_id;
    return micropixel::Send(wire, payload);
}
Result<AppMessage> Messages::Read(uint32_t ticket) const {
    micropixel_handle_request_t request{sizeof(request), 0, ticket};
    micropixel_message_received_t response{};
    uint32_t written = 0;
    auto status = Call(messages, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_READ, &request, sizeof(request),
                       &response, sizeof(response), written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    if (written != sizeof(response) || response.size != sizeof(response) ||
        response.payload_length > sizeof(response.payload) || response.reserved0 || response.reserved1)
        return unexpected(Error{ErrorCode::kInternal});
    size_t name_length = 0;
    while (name_length < sizeof(response.source_app) && response.source_app[name_length]) ++name_length;
    if (!name_length || name_length > 64) return unexpected(Error{ErrorCode::kInternal});
    AppMessage message;
    message.type = response.type;
    message.source = {response.source_boot, response.source_slot, response.source_epoch, response.source_component};
    message.sequence = response.sequence;
    message.length = response.payload_length;
    message.source_app.Append(response.source_app);
    runtime::CopyBytes(message.payload.data(), response.payload, message.length);
    return message;
}
Result<void> Messages::Ack(uint32_t ticket, int32_t result) const {
    micropixel_message_ack_t request{sizeof(request), 0, ticket, result};
    uint32_t written = 0;
    auto status = Call(messages, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_ACK, &request, sizeof(request), nullptr,
                       0, written);
    return status ? Result<void>(unexpected(runtime::ErrorFromStatus(status))) : Result<void>();
}
Result<DeliveryResult> Messages::ResultOf(uint32_t ticket) const {
    micropixel_handle_request_t request{sizeof(request), 0, ticket};
    micropixel_message_result_t response{};
    uint32_t written = 0;
    auto status = Call(messages, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_RESULT, &request, sizeof(request),
                       &response, sizeof(response), written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    if (written != sizeof(response) || response.size != sizeof(response) || response.complete > 1)
        return unexpected(Error{ErrorCode::kInternal});
    return DeliveryResult{response.complete != 0, response.result};
}
}  // namespace micropixel
