// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace micropixel::pc {
// All view methods run under the LVGL adapter lock. The owned worker never calls LVGL.
class Market final {
   public:
    Market();
    ~Market();
    Market(const Market&) = delete;
    Market& operator=(const Market&) = delete;
    void Show(const std::string& app_id = {});
    void Hide();
    void Update();
    void Back();
    bool Visible() const;
    bool TakeHallRequest();
    std::filesystem::path TakeLaunchRequest();
    void ReportLaunchError(const std::string& error);

   private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace micropixel::pc
