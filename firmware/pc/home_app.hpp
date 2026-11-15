// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <vector>

#include "lvgl.h"
#include "media.hpp"
#include "portable_app.hpp"

namespace micropixel::pc {
// Host system application. All methods run on the UI thread under the LVGL lock.
class HomeApp final {
   public:
    HomeApp(std::vector<std::unique_ptr<PortableApp>>& apps, MediaPlayer& media, SdlAudio& audio);
    ~HomeApp();
    void Show(int width, int height);
    void Hide();
    void Update();
    void SetRunning(const PortableApp* app);
    bool Visible() const { return root_ != nullptr; }
    int requested{-1};
    bool market_requested{}, music_requested{}, resume_requested{}, close_requested{};

   private:
    static void Launch(lv_event_t*);
    static void Action(lv_event_t*);
    std::vector<std::unique_ptr<PortableApp>>& apps_;
    MediaPlayer& media_;
    SdlAudio& audio_;
    const PortableApp* running_{};
    lv_obj_t *root_{}, *catalog_{}, *running_title_{}, *status_{}, *resume_{}, *pause_{}, *close_{};
    int32_t scroll_y_{};
    int64_t updated_at_{};
};
}  // namespace micropixel::pc
