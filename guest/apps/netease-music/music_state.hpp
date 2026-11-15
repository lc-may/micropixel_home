// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
namespace netease {
enum class MusicScreen { kLibrary, kDiscover, kSearch, kCollection, kPlayer };
enum class MusicContent { kPlaylists, kRecommendations, kSearch, kPlaylist, kLiked };
enum class MusicActionKind {
    kNone,
    kLibrary,
    kDiscover,
    kSearch,
    kAccount,
    kRefresh,
    kLiked,
    kRow,
    kFavorite,
    kPlayAll,
    kLoadMore,
    kOpenPlayer,
    kBack,
    kPrevious,
    kToggle,
    kNext,
    kStop,
    kSeek,
    kEditQuery,
    kSubmit
};
struct MusicAction {
    MusicActionKind kind{};
    uint32_t index{}, value{};
};
struct MusicState {
    MusicScreen screen{MusicScreen::kSearch};
    MusicContent content{MusicContent::kSearch};
    std::string collection, query{"纯音乐"}, notice{"搜索喜欢的音乐"}, load_hint;
    uint32_t batch{}, revision{};
    uint64_t favorite_pending{};
    bool loading{}, failed{}, editing{}, has_more{}, appending{};
};
}  // namespace netease
