// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <utility>

#include "sdk/graphics.hpp"
#include "sdk/scene.hpp"

namespace micropixel::ui {
template <class Action>
struct UiUpdate final {
    bool handled{}, changed{};
    std::optional<Action> action{};
};

// Scene owns the nodes; this move-only object owns subtree cleanup.
class ComponentRoot final {
   public:
    ComponentRoot() = default;
    explicit ComponentRoot(ContainerNode node) : node_(node) {}
    ComponentRoot(const ComponentRoot&) = delete;
    ComponentRoot& operator=(const ComponentRoot&) = delete;
    ComponentRoot(ComponentRoot&& other) noexcept : node_(std::exchange(other.node_, {})) {}
    ComponentRoot& operator=(ComponentRoot&& other) noexcept {
        if (this != &other) {
            Destroy();
            node_ = std::exchange(other.node_, {});
        }
        return *this;
    }
    ~ComponentRoot() { Destroy(); }
    bool valid() const { return node_.valid(); }
    ContainerNode& node() { return node_; }
    const ContainerNode& node() const { return node_; }
    void Destroy() {
        if (node_.valid()) (void)node_.Destroy();
        node_ = {};
    }

   private:
    ContainerNode node_;
};
}  // namespace micropixel::ui
