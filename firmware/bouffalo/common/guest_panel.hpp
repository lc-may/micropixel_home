// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

#include "platform/graphics/guest_scene.hpp"
#include "runtime/abi/service_registry.hpp"

struct _lv_obj_t;
struct _lv_font_t;

namespace micropixel::runtime {
class BitmapStore;
class EventQueue;
}  // namespace micropixel::runtime

namespace micropixel::bouffalo {

class GuestPanel final : public runtime::ServiceHandler {
   public:
    static constexpr uint32_t kExtent = 480;
    static constexpr uint32_t kNavigationHeight = 44;
    static constexpr uint32_t kContentHeight = kExtent - kNavigationHeight;
    bool Initialize(uint16_t canvas_extent);
    void Release();
    void SetVisible(bool visible);
    void Render(_lv_obj_t* surface);
    void Reset();
    bool HasFrame() const;
    uint16_t Width() const;
    uint16_t Height() const;
    void MapTouch(int32_t& x, int32_t& y) const;
    runtime::BitmapStore& Bitmaps();
    // Set during Host initialization, before the first Guest is started.
    static void SetFontFallback(const _lv_font_t& font);
    static const _lv_font_t* Font(micropixel_font_handle_t font);

    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;
    int32_t Submit(uint32_t channel, const uint8_t* bytes, uint32_t length) override;

   private:
    struct Storage;
    Storage* storage_{};
};

class TouchEndpoint final : public runtime::ServiceHandler {
   public:
    TouchEndpoint(runtime::EventQueue& events, int64_t origin_us, const GuestPanel& panel)
        : events_(events), origin_us_(origin_us), panel_(panel) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;
    bool WantsText() const;
    bool SubmitText(std::string_view text);
    void ClearText();

   private:
    runtime::EventQueue& events_;
    int64_t origin_us_;
    const GuestPanel& panel_;
    char text_[128];
    uint32_t read_{}, end_{}, key_sequence_{};
    bool enabled_{}, submitted_{};
};

}  // namespace micropixel::bouffalo
