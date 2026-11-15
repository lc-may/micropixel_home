// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <optional>

#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"
namespace {
using namespace micropixel;
uint32_t widget_service{}, message_service{};
int32_t Call(uint32_t service, uint32_t method, const void* request, uint32_t size, void* output = nullptr,
             uint32_t capacity = 0) {
    uint32_t written = 0;
    return micropixel_service_call(service, method, static_cast<const uint8_t*>(request), size,
                                   static_cast<uint8_t*>(output), capacity, &written);
}
class Page final : public Activity {
    void OnCreate(ComponentContext& context) override {
        scene_.emplace(context.app().renderer().CreateScene(Color::White()).value());
        context.app().renderer().Present(*scene_).value();
    }
    std::optional<Scene> scene_;
};
class Service final : public AppService {
    void OnStart(ComponentContext& context) override {
        const auto& app = context.app();
        micropixel_service_info_t info{};
        Assert(micropixel_service_open(MICROPIXEL_SERVICE_WIDGET, 65536, &info, sizeof(info)) == 0 && info.capabilities,
               "widget endpoint");
        widget_service = info.service_handle;
        Assert(
            micropixel_service_open(MICROPIXEL_SERVICE_MESSAGE, 65536, &info, sizeof(info)) == 0 && info.capabilities,
            "message endpoint");
        message_service = info.service_handle;
        micropixel_widget_snapshot_t snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.template_id = MICROPIXEL_WIDGET_MEDIA;
        snapshot.widget_id = 1;
        snapshot.revision = 1;
        std::memcpy(snapshot.title, "Wire contract", 14);
        micropixel_handle_response_t response{};
        snapshot.reserved0 = 1;
        Assert(Call(widget_service, MICROPIXEL_WIDGET_PUBLISH, &snapshot, sizeof(snapshot), &response,
                    sizeof(response)) == MICROPIXEL_STATUS_INVALID_ARGUMENT,
               "widget reserved bits");
        snapshot.reserved0 = 0;
        Assert(Call(widget_service, MICROPIXEL_WIDGET_PUBLISH, &snapshot, sizeof(snapshot), &response,
                    sizeof(response) - 1) == MICROPIXEL_STATUS_BUFFER_TOO_SMALL,
               "widget short output");
        (void)app.widgets().Publish(1, {.title = "Wire contract"}).value();
        Assert(Call(widget_service, MICROPIXEL_WIDGET_PUBLISH, &snapshot, sizeof(snapshot), &response,
                    sizeof(response)) == MICROPIXEL_STATUS_STALE_STATE,
               "duplicate widget revision");
        if (app.launch_arguments().HasFlag("--bad-pointer")) {
            (void)Call(widget_service, MICROPIXEL_WIDGET_PUBLISH, reinterpret_cast<const void*>(UINTPTR_MAX),
                       sizeof(snapshot), &response, sizeof(response));
            Assert(false, "invalid Guest pointer was accepted");
        }
        Assert(Call(message_service, 999, nullptr, 0) == MICROPIXEL_STATUS_UNSUPPORTED, "unknown message method");
        timer_.emplace(app.timers().Every(Duration::Milliseconds(10)).value());
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (timer_ && event.TimerFrom(*timer_) && !sent_) {
            sent_ = true;
            auto ticket =
                app.messages().Send("test.app-services-contract", static_cast<uint16_t>(AppAction::kRefresh)).value();
            ticket_ = ticket;
        }
        if (const auto* incoming = event.message()) {
            const auto message = app.messages().Read(incoming->ticket).value();
            Assert(std::string_view(message.source_app.c_str()) == "test.app-services-contract" &&
                       message.source.component == context.token() && message.source.boot && message.source.epoch,
                   "Host-injected message origin");
            ++delivered_;
            app.messages().Ack(incoming->ticket).value();
            Assert(app.messages().ResultOf(incoming->ticket).value().complete, "message result acknowledgement");
            if (delivered_ == 1) {
                micropixel_message_send_t wire{};
                wire.size = sizeof(wire);
                wire.type = MICROPIXEL_ACTION_NEXT_TRACK;
                wire.flags = MICROPIXEL_MESSAGE_LOGICAL;
                wire.sequence = 100;
                wire.ttl_ms = 1000;
                std::memcpy(wire.target_app, "test.app-services-contract", sizeof("test.app-services-contract"));
                micropixel_handle_response_t first{}, duplicate{};
                wire.reserved1 = 1;
                Assert(Call(message_service, MICROPIXEL_MESSAGE_SEND, &wire, sizeof(wire), &first, sizeof(first)) ==
                           MICROPIXEL_STATUS_INVALID_ARGUMENT,
                       "message reserved bits");
                wire.reserved1 = 0;
                Assert(Call(message_service, MICROPIXEL_MESSAGE_SEND, &wire, sizeof(wire), &first, sizeof(first)) == 0,
                       "first message");
                Assert(Call(message_service, MICROPIXEL_MESSAGE_SEND, &wire, sizeof(wire), &duplicate,
                            sizeof(duplicate)) == 0 &&
                           first.handle == duplicate.handle,
                       "duplicate Next ticket");
                Assert(app.messages()
                               .Send("test.app-services-contract", static_cast<uint16_t>(AppAction::kNextTrack), {},
                                     1000, 100)
                               .value() == first.handle,
                       "SDK explicit request-id retry");
            } else {
                Assert(delivered_ == 2, "duplicate Next executed");
                app.log().Info(
                    "APP SERVICES CONTRACT PASS wire validation, origin injection, read/ack/result and duplicate "
                    "command suppression");
                context.Exit().value();
            }
        }
    }
    void OnStop(ComponentContext&) override { timer_.reset(); }
    std::optional<Timer> timer_;
    uint32_t ticket_{}, delivered_{};
    bool sent_{};
};
}  // namespace
int main() {
    Application app;
    Page page;
    Service service;
    app.RunComponents(&page, &service);
    return 0;
}
