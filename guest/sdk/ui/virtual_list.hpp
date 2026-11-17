// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <memory>
#include <new>

#include "sdk/ui/component.hpp"
#include "sdk/ui/scroll_view.hpp"

namespace micropixel::ui {
enum class ItemRegion { kBody, kTrailing };
struct ItemAction final {
    uint64_t key{};
    uint32_t generation{}, index{};
    ItemRegion region{};
};
struct RowUpdate final {
    bool changed{}, clicked{}, action_clicked{};
};
struct ListGeometry final {
    Rect bounds{};
    int32_t item_width{}, item_height{}, column_gap{}, row_gap{};
    uint32_t columns{1};
};
struct VirtualListUpdate final {
    bool handled{}, changed{}, drag_ended{};
    std::optional<ItemAction> action{};
};

// Fixed visible pool. Row factories/binders remain typed and never cross ABI.
template <class Row, size_t Capacity>
class VirtualList final {
   public:
    template <class CreateRow>
    static Result<VirtualList> Create(Container& parent, ListGeometry geometry, CreateRow&& create) {
        if (!ValidGeometry(geometry)) return unexpected(Error{ErrorCode::kInvalidArgument});
        auto root = parent.CreateContainer();
        if (!root) return unexpected(root.error());
        VirtualList result;
        result.root_ = ComponentRoot(*root);
        result.pool_.reset(new (std::nothrow) Pool);
        if (!result.pool_) return unexpected(Error{ErrorCode::kResourceExhausted});
        result.geometry_ = geometry;
        result.scroll_ = result.root_.node().CreateScrollView(geometry.bounds);
        for (auto& row : result.pool_->rows_) {
            row = create(result.scroll_.content(), Rect{0, 0, geometry.item_width, geometry.item_height});
            row.SetVisible(false);
        }
        return result;
    }
    bool valid() const { return root_.valid(); }
    int32_t offset() const { return scroll_.offset(); }
    bool at_end(int32_t threshold = 0) const { return scroll_.at_end(threshold); }
    bool contains(Point point) const { return valid() && visible_ && scroll_.contains(point); }
    Result<void> SetBounds(Rect bounds) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        auto geometry = geometry_;
        geometry.bounds = bounds;
        if (!ValidGeometry(geometry)) return unexpected(Error{ErrorCode::kInvalidArgument});
        if (geometry.bounds != geometry_.bounds) {
            CancelInput();
            geometry_ = geometry;
            scroll_.SetBounds(bounds);
        }
        return {};
    }
    void ScrollTo(int32_t offset) {
        if (!valid()) return;
        CancelInput();
        scroll_.ScrollTo(offset);
    }
    void SetVisible(bool visible) {
        if (!valid()) return;
        if (!visible) CancelInput();
        visible_ = visible;
        root_.node().SetVisible(visible);
    }
    void CancelInput() {
        tracking_ = false;
        captured_.reset();
        if (!valid()) return;
        scroll_.Cancel();
        for (auto& row : pool_->rows_) row.Reset();
    }
    // Replace: advance generation and optionally reset offset. Append: keep it.
    Result<void> SetItems(uint32_t count, uint32_t generation, bool reset_offset = false) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        const uint64_t lines = (uint64_t(count) + geometry_.columns - 1) / geometry_.columns;
        const uint64_t height =
            lines ? lines * uint64_t(geometry_.item_height) + (lines - 1) * uint64_t(geometry_.row_gap) : 0;
        if (height > INT32_MAX) return unexpected(Error{ErrorCode::kInvalidArgument});
        if (count_ != count || generation_ != generation || reset_offset) CancelInput();
        count_ = count;
        generation_ = generation;
        scroll_.SetContentHeight(int32_t(height));
        if (reset_offset) scroll_.ScrollTo(0);
        return {};
    }
    // A binder updates existing nodes; key must be unique and stable in a dataset.
    template <class Key, class Bind>
    void BindRows(Key&& key, Bind&& bind, bool refresh = false) {
        if (!valid()) return;
        const uint32_t first = uint32_t(offset() / (geometry_.item_height + geometry_.row_gap)) * geometry_.columns;
        for (uint32_t slot = 0; slot < Capacity; ++slot) {
            auto& row = pool_->rows_[slot];
            const uint32_t index = first + slot;
            if (index >= count_ || index < first) {
                if (pool_->indices_[slot] != UINT32_MAX) row.Reset();
                pool_->indices_[slot] = UINT32_MAX;
                row.SetVisible(false);
                continue;
            }
            const uint64_t identity = key(index);
            for (uint32_t previous = 0; previous < slot; ++previous)
                Assert(pool_->indices_[previous] == UINT32_MAX || pool_->keys_[previous] != identity,
                       "duplicate visible item key");
            const bool rebound = pool_->indices_[slot] != index || pool_->keys_[slot] != identity ||
                                 pool_->generations_[slot] != generation_;
            if (rebound) {
                row.Reset();
                if (captured_ && captured_->slot == slot) captured_.reset();
            }
            pool_->indices_[slot] = index;
            pool_->keys_[slot] = identity;
            pool_->generations_[slot] = generation_;
            row.SetVisible(true);
            const Rect bounds{int32_t(index % geometry_.columns) * (geometry_.item_width + geometry_.column_gap),
                              int32_t(index / geometry_.columns) * (geometry_.item_height + geometry_.row_gap),
                              geometry_.item_width, geometry_.item_height};
            if (refresh || rebound) bind(row, index, bounds);
        }
    }
    template <class Key, class Bind, class Touch>
    VirtualListUpdate OnTouch(const TouchEvent& event, Key&& key, Bind&& bind, Touch&& touch) {
        if (!valid() || !visible_) return {};
        if (event.phase() == TouchPhase::kDown) {
            if (tracking_ || !contains(event.position())) return {};
            tracking_ = true;
            touch_id_ = event.id();
            captured_.reset();
        }
        if (!tracking_ || event.id() != touch_id_) return {};
        BindRows(key, bind);
        auto scrolling = scroll_.OnTouch(event);
        VirtualListUpdate result{.handled = true, .changed = scrolling.changed};
        if (scrolling.dragging) {
            result.changed = true;  // Resetting pressed feedback also needs a submission at a scroll boundary.
            captured_.reset();
            for (auto& row : pool_->rows_) row.Reset();
            BindRows(key, bind);
            result.drag_ended = event.phase() == TouchPhase::kUp;
        } else {
            for (uint32_t slot = 0; slot < Capacity; ++slot) {
                if (pool_->indices_[slot] == UINT32_MAX) continue;
                auto update = touch(pool_->rows_[slot], event);
                result.changed |= update.changed;
                if (event.phase() == TouchPhase::kDown) {
                    // Only the row under Down may produce a later action.
                    const auto p = scroll_.content().ToLocal(event.position());
                    const auto index = pool_->indices_[slot];
                    const Rect bounds{
                        int32_t(index % geometry_.columns) * (geometry_.item_width + geometry_.column_gap),
                        int32_t(index / geometry_.columns) * (geometry_.item_height + geometry_.row_gap),
                        geometry_.item_width, geometry_.item_height};
                    if (bounds.contains(p)) captured_ = Capture{slot, pool_->keys_[slot], generation_};
                }
                if ((update.clicked || update.action_clicked) && captured_ && captured_->slot == slot &&
                    captured_->key == pool_->keys_[slot] && captured_->generation == generation_ &&
                    contains(event.position()))
                    result.action = ItemAction{pool_->keys_[slot], generation_, pool_->indices_[slot],
                                               update.action_clicked ? ItemRegion::kTrailing : ItemRegion::kBody};
            }
        }
        if (event.phase() == TouchPhase::kUp || event.phase() == TouchPhase::kCancel) {
            tracking_ = false;
            captured_.reset();
            for (auto& row : pool_->rows_) row.Reset();
        }
        return result;
    }
    void Destroy() {
        CancelInput();
        root_.Destroy();
        pool_.reset();
    }

   private:
    static bool ValidGeometry(const ListGeometry& g) {
        if (g.bounds.empty() || g.columns == 0 || g.item_width <= 0 || g.item_height <= 0 || g.row_gap < 0 ||
            g.column_gap < 0 || int64_t(g.item_height) + g.row_gap > INT32_MAX ||
            int64_t(g.item_width) + g.column_gap > INT32_MAX ||
            int64_t(g.item_width) * g.columns + int64_t(g.column_gap) * (g.columns - 1) > g.bounds.width)
            return false;
        const uint64_t needed =
            (uint64_t(g.bounds.height) + g.item_height + g.row_gap - 1) / uint64_t(g.item_height + g.row_gap) + 1;
        return needed * g.columns <= Capacity;
    }
    struct Capture {
        uint32_t slot;
        uint64_t key;
        uint32_t generation;
    };
    ComponentRoot root_;
    ScrollView scroll_;
    struct Pool final {
        std::array<Row, Capacity> rows_;
        std::array<uint64_t, Capacity> keys_{};
        std::array<uint32_t, Capacity> generations_{};
        std::array<uint32_t, Capacity> indices_ = [] {
            std::array<uint32_t, Capacity> values{};
            values.fill(UINT32_MAX);
            return values;
        }();
    };
    std::unique_ptr<Pool> pool_;
    std::optional<Capture> captured_;
    ListGeometry geometry_;
    uint32_t count_{}, generation_{}, touch_id_{};
    bool visible_{true}, tracking_{};
};
}  // namespace micropixel::ui
