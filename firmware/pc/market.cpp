// SPDX-License-Identifier: Apache-2.0
#include "market.hpp"

#include <cjson/cJSON.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "network.hpp"
#include "runtime/resources/bitmap_decoder.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "store.hpp"

namespace micropixel::pc {
namespace {
constexpr size_t kPageSize = 4, kMaxApps = 128, kMaxReleases = 32;
constexpr uint32_t kCoverWidth = 160, kCoverHeight = 96;
enum class Kind { kCatalog, kDetail, kCover, kInstall };
struct Release {
    std::string version, target, type, notes, id, sha256;
    uint32_t size{};
};
struct App {
    std::string id, title, summary, description, developer, category, cover;
    std::vector<Release> releases;
};
struct Request {
    Kind kind{};
    uint64_t sequence{}, view{};
    std::string key, url;
    StoreRelease release;
};
struct Response {
    Request request;
    std::string error;
    std::vector<App> apps;
    App detail;
    std::vector<uint8_t> pixels;
    std::filesystem::path installed;
};
std::string Text(const cJSON* object, const char* key, size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring) return {};
    const size_t size = std::strlen(value->valuestring);
    if (size > limit) return {};
    return {value->valuestring, size};
}
bool ValidId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") ==
               std::string::npos;
}
bool ReadApp(const cJSON* object, App& app) {
    if (!cJSON_IsObject(object)) return false;
    app.id = Text(object, "appId", 128);
    app.title = Text(object, "title", 512);
    if (!ValidId(app.id) || app.title.empty()) return false;
    app.summary = Text(object, "summary", 2048);
    app.description = Text(object, "description", 16384);
    app.developer = Text(object, "developer", 256);
    app.category = Text(object, "category", 128);
    app.cover = Text(object, "cover", 2048);
    const auto* releases = cJSON_GetObjectItemCaseSensitive(object, "releases");
    if (releases && !cJSON_IsArray(releases)) return false;
    const cJSON* release;
    cJSON_ArrayForEach(release, releases) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "withdrawn"))) continue;
        if (app.releases.size() == kMaxReleases) break;
        Release item;
        item.version = Text(release, "version", 128);
        item.target = Text(release, "target", 64);
        item.type = Text(release, "packageType", 64);
        item.notes = Text(release, "releaseNotes", 8192);
        item.id = Text(release, "id", 64);
        item.sha256 = Text(release, "sha256", 64);
        const auto* size = cJSON_GetObjectItemCaseSensitive(release, "sizeBytes");
        if (cJSON_IsNumber(size) && size->valuedouble >= 0 && size->valuedouble <= 128U * 1024 * 1024)
            item.size = static_cast<uint32_t>(size->valuedouble);
        if (!item.version.empty() && !item.target.empty()) app.releases.push_back(std::move(item));
    }
    return true;
}
bool DecodeCover(const std::vector<uint8_t>& body, std::vector<uint8_t>& pixels) {
    micropixel_bundle_asset_view_t asset{};
    asset.data = body.data();
    asset.size = body.size();
    if (body.size() >= 8 && std::memcmp(body.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        asset.format = MICROPIXEL_BUNDLE_FORMAT_PNG;
    else if (body.size() >= 2 && body[0] == 0xff && body[1] == 0xd8)
        asset.format = MICROPIXEL_BUNDLE_FORMAT_JPEG;
    else
        return false;
    runtime::DecodedBitmap bitmap;
    if (!runtime::DecodeBitmap(asset, bitmap)) return false;
    const auto& view = bitmap.view();
    const uint32_t bytes = view.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888 ? 3 : 4;
    if (bytes == 4 && view.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888) return false;
    pixels.resize(kCoverWidth * kCoverHeight * 4);
    // Fit the entire cover inside a fixed thumbnail, preserving its aspect ratio.
    std::fill(pixels.begin(), pixels.end(), 0);
    const double scale = std::min(double(kCoverWidth) / view.width, double(kCoverHeight) / view.height);
    const uint32_t width = std::max(1U, static_cast<uint32_t>(view.width * scale));
    const uint32_t height = std::max(1U, static_cast<uint32_t>(view.height * scale));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto* src = view.data + (uint64_t(y) * view.height / height) * view.stride +
                              (uint64_t(x) * view.width / width) * bytes;
            auto* dst =
                pixels.data() + ((y + (kCoverHeight - height) / 2) * kCoverWidth + x + (kCoverWidth - width) / 2) * 4;
            std::memcpy(dst, src, 3);
            dst[3] = bytes == 4 ? src[3] : 255;
        }
    }
    return true;
}
// One active request, one replaceable pending request, and one completion slot.
// Replacing a request cancels socket I/O without waiting on the UI thread.
class Worker final {
   public:
    Worker() : thread_([this] { Run(); }) {}
    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            cancel_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }
    void Submit(Kind kind, std::string key, std::string url, uint64_t view, const StoreRelease& release = {}) {
        {
            std::lock_guard lock(mutex_);
            cancel_ = true;
            pending_ = Request{kind, ++sequence_, view, std::move(key), std::move(url), release};
            completed_.reset();
        }
        wake_.notify_one();
    }
    void Cancel() {
        std::lock_guard lock(mutex_);
        ++sequence_;
        cancel_ = true;
        pending_.reset();
        completed_.reset();
    }
    bool Busy() {
        std::lock_guard lock(mutex_);
        return working_ || pending_.has_value() || completed_ != nullptr;
    }
    std::unique_ptr<Response> Poll() {
        std::lock_guard lock(mutex_);
        return std::move(completed_);
    }

   private:
    void Run() {
        for (;;) {
            auto result = std::make_unique<Response>();
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
                if (stop_) return;
                result->request = std::move(*pending_);
                pending_.reset();
                cancel_ = false;
                working_ = true;
            }
            Fetch(*result);
            std::lock_guard lock(mutex_);
            if (result->request.sequence == sequence_ && !stop_) completed_ = std::move(result);
            working_ = false;
        }
    }
    void Fetch(Response& result) {
        if (result.request.kind == Kind::kInstall) {
            InstallRelease(result.request.url, result.request.release, result.installed, result.error, &cancel_);
            return;
        }
        std::vector<uint8_t> body;
        if (!HttpGet(result.request.url, body, result.error, &cancel_, 2U * 1024 * 1024)) return;
        if (cancel_) return;
        if (result.request.kind == Kind::kCover) {
            if (!DecodeCover(body, result.pixels)) result.error = "Unsupported or invalid cover image";
            return;
        }
        body.push_back(0);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
            cJSON_ParseWithLengthOpts(reinterpret_cast<const char*>(body.data()), body.size(), nullptr, true),
            cJSON_Delete);
        if (!json) {
            result.error = "Invalid market JSON response";
            return;
        }
        if (result.request.kind == Kind::kDetail) {
            if (!ReadApp(json.get(), result.detail) || result.detail.id != result.request.key)
                result.error = "Invalid market app details";
            return;
        }
        const auto* apps = cJSON_GetObjectItemCaseSensitive(json.get(), "apps");
        if (!cJSON_IsArray(apps) || cJSON_GetArraySize(apps) > static_cast<int>(kMaxApps)) {
            result.error = "Invalid or oversized market catalog (maximum 128 entries)";
            return;
        }
        const cJSON* app;
        cJSON_ArrayForEach(app, apps) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(app, "hidden"))) continue;
            App item;
            if (!ReadApp(app, item) ||
                std::any_of(result.apps.begin(), result.apps.end(), [&](const App& a) { return a.id == item.id; })) {
                result.error = "Invalid or duplicate market entry";
                result.apps.clear();
                return;
            }
            result.apps.push_back(std::move(item));
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::unique_ptr<Response> completed_;
    std::atomic<bool> cancel_{};
    bool working_{}, stop_{};
    uint64_t sequence_{};
    std::thread thread_;
};
lv_obj_t* Label(lv_obj_t* parent, const std::string& text, uint32_t color = 0xe8eef7) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}
lv_obj_t* Row(lv_obj_t* parent, int height) {
    auto* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    return row;
}
lv_obj_t* Button(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* context) {
    auto* button = lv_button_create(parent);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x194e77), 0);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    return button;
}
std::string Lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
}  // namespace

struct Market::State {
    struct Cover {
        std::string url;
        std::vector<uint8_t> pixels;
        lv_image_dsc_t descriptor{};
        lv_obj_t *image{}, *placeholder{};
        bool attempted{};
    };
    Worker worker;
    std::string base = "https://micropixel.ai", selected, query;
    std::vector<App> apps;
    std::optional<App> detail;
    std::array<Cover, kPageSize> covers;
    lv_obj_t *root{}, *content{}, *status{}, *search{}, *back{}, *pager{};
    lv_obj_t* install_button{};
    std::filesystem::path launch_path;
    size_t page{};
    uint64_t view{};
    bool loaded{}, hall_requested{};

    State() {
        if (const char* value = std::getenv("MICROPIXEL_PC_MARKET_URL")) base = value;
        while (base.ends_with('/')) base.pop_back();
    }
    ~State() { Hide(); }
    static State& Self(lv_event_t* event) { return *static_cast<State*>(lv_event_get_user_data(event)); }
    static void BackClick(lv_event_t* event) { Self(event).Back(); }
    static void RefreshClick(lv_event_t* event) { Self(event).Refresh(); }
    const Release* Rv32Release() const {
        if (!detail) return nullptr;
        for (const auto& release : detail->releases)
            if (release.target == "riscv32-ilp32f" && !release.id.empty() && release.sha256.size() == 64)
                return &release;
        return nullptr;
    }
    static void InstallClick(lv_event_t* event) {
        auto& state = Self(event);
        const auto* release = state.Rv32Release();
        if (!release) return;
        StoreRelease item{release->id, state.detail->id, release->version, release->sha256, release->size};
        state.worker.Submit(Kind::kInstall, state.selected, state.base, state.view, item);
        lv_obj_add_state(state.install_button, LV_STATE_DISABLED);
        state.SetStatus("正在下载并校验 RISC-V 应用包...");
    }
    static void SearchChange(lv_event_t* event) {
        auto& state = Self(event);
        state.query = Lower(lv_textarea_get_text(state.search));
        state.page = 0;
        state.BuildList();
    }
    static void PageClick(lv_event_t* event) {
        auto& state = Self(event);
        const auto direction = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
        if (direction < 0 && state.page > 0) --state.page;
        if (direction > 0) ++state.page;
        state.BuildList();
    }
    static void AppClick(lv_event_t* event) {
        auto& state = Self(event);
        const auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
        if (index < state.apps.size()) state.Open(state.apps[index].id);
    }
    std::string CoverUrl(const std::string& url) const {
        if (url.starts_with('/') && !url.starts_with("//")) return base + url;
        if (url.starts_with("https://")) return url;
        return {};
    }
    void SetStatus(const std::string& text, bool error = false) {
        if (!status) return;
        lv_label_set_text(status, text.c_str());
        lv_obj_set_style_text_color(status, lv_color_hex(error ? 0xffb88c : 0x91a4bd), 0);
    }
    void ClearContent() {
        ++view;
        if (content) lv_obj_clean(content);
        install_button = nullptr;
        for (auto& cover : covers) {
            if (cover.descriptor.data) {
                lv_image_cache_drop(&cover.descriptor);
                lv_image_header_cache_drop(&cover.descriptor);
            }
            cover = {};
        }
    }
    void Hide() {
        if (root) lv_obj_delete(root);
        root = content = status = search = back = pager = nullptr;
        ClearContent();
        worker.Cancel();
    }
    void Show(const std::string& app_id) {
        Hide();
        selected.clear();
        launch_path.clear();
        hall_requested = false;
        root = lv_obj_create(lv_layer_top());
        lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(root, lv_color_hex(0x08111f), 0);
        lv_obj_set_style_border_width(root, 0, 0);
        lv_obj_set_style_pad_all(root, 20, 0);
        lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
        auto* header = Row(root, 48);
        back = Button(header, "本地应用", BackClick, this);
        auto* title = Label(header, "应用市场");
        lv_obj_set_flex_grow(title, 1);
        Button(header, "刷新", RefreshClick, this);
        Label(root, base == "https://micropixel.ai" ? "micropixel.ai  /  官方应用市场" : base, 0x6ddac8);
        search = lv_textarea_create(root);
        lv_obj_set_size(search, LV_PCT(100), 44);
        lv_textarea_set_one_line(search, true);
        lv_textarea_set_max_length(search, 128);
        lv_textarea_set_placeholder_text(search, "搜索应用名称 / App ID");
        lv_textarea_set_text(search, query.c_str());
        lv_obj_add_event_cb(search, SearchChange, LV_EVENT_VALUE_CHANGED, this);
        status = Label(root, "正在连接市场...");
        content = lv_obj_create(root);
        lv_obj_set_width(content, LV_PCT(100));
        lv_obj_set_flex_grow(content, 1);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        if (!app_id.empty())
            Open(app_id);
        else if (loaded)
            BuildList();
        else
            Refresh();
    }
    void Back() {
        if (!selected.empty()) {
            selected.clear();
            detail.reset();
            worker.Cancel();
            lv_obj_set_hidden(search, false);
            lv_label_set_text(lv_obj_get_child(back, 0), "本地应用");
            if (loaded)
                BuildList();
            else
                Refresh();
        } else {
            hall_requested = true;
        }
    }
    void Refresh() {
        ClearContent();
        SetStatus("正在连接市场...");
        if (selected.empty()) {
            worker.Submit(Kind::kCatalog, {}, base + "/api/v1/store/apps", view);
        } else {
            worker.Submit(Kind::kDetail, selected, base + "/api/v1/store/apps/" + selected, view);
        }
    }
    void AddCover(lv_obj_t* parent, Cover& cover, const std::string& url) {
        cover.url = CoverUrl(url);
        auto* box = lv_obj_create(parent);
        lv_obj_set_size(box, LV_PCT(100), kCoverHeight);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x102a40), 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_scrollable(box, false);
        lv_obj_set_clickable(box, false);
        cover.placeholder = lv_label_create(box);
        lv_label_set_text(cover.placeholder, url.empty() ? "MicroPixel" : "封面加载中");
        lv_obj_set_style_text_color(cover.placeholder, lv_color_hex(0x91a4bd), 0);
        lv_obj_center(cover.placeholder);
        cover.image = lv_image_create(box);
        lv_obj_center(cover.image);
        lv_obj_set_clickable(cover.placeholder, false);
        lv_obj_set_clickable(cover.image, false);
    }
    void BuildList() {
        if (!content) return;
        ClearContent();
        std::array<size_t, kMaxApps> matches{};
        size_t count = 0;
        for (size_t i = 0; i < apps.size(); ++i) {
            if (query.empty() ||
                Lower(apps[i].title + " " + apps[i].summary + " " + apps[i].id).find(query) != std::string::npos)
                matches[count++] = i;
        }
        const size_t pages = std::max(size_t{1}, (count + kPageSize - 1) / kPageSize);
        page = std::min(page, pages - 1);
        SetStatus("共 " + std::to_string(apps.size()) + " 项  /  当前匹配 " + std::to_string(count) + " 项");
        if (!count) Label(content, "没有找到应用。可修改搜索条件或点击刷新。");
        lv_obj_t* row = nullptr;
        for (size_t slot = 0; slot < kPageSize && page * kPageSize + slot < count; ++slot) {
            const size_t index = matches[page * kPageSize + slot];
            const auto& app = apps[index];
            if (slot % 2 == 0) row = Row(content, LV_SIZE_CONTENT);
            auto* card = lv_button_create(row);
            lv_obj_set_width(card, LV_PCT(48));
            lv_obj_set_height(card, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x142338), 0);
            lv_obj_set_style_pad_all(card, 12, 0);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_user_data(card, reinterpret_cast<void*>(index));
            lv_obj_add_event_cb(card, AppClick, LV_EVENT_CLICKED, this);
            AddCover(card, covers[slot], app.cover);
            auto* title = Label(card, app.title);
            lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
            lv_obj_set_height(title, 28);
            auto* summary = Label(card, app.summary.empty() ? app.id : app.summary, 0x91a4bd);
            lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
            lv_obj_set_height(summary, 27);
        }
        pager = Row(content, 42);
        auto* previous = Button(pager, "上一页", PageClick, this);
        lv_obj_set_user_data(previous, reinterpret_cast<void*>(intptr_t{-1}));
        if (page == 0) lv_obj_add_state(previous, LV_STATE_DISABLED);
        auto* number = Label(pager, std::to_string(page + 1) + " / " + std::to_string(pages));
        lv_obj_set_flex_grow(number, 1);
        auto* next = Button(pager, "下一页", PageClick, this);
        lv_obj_set_user_data(next, reinterpret_cast<void*>(intptr_t{1}));
        if (page + 1 == pages) lv_obj_add_state(next, LV_STATE_DISABLED);
    }
    void Open(const std::string& id) {
        if (!ValidId(id)) {
            SetStatus("无效的 App ID", true);
            return;
        }
        selected = id;
        detail.reset();
        lv_obj_set_hidden(search, true);
        lv_label_set_text(lv_obj_get_child(back, 0), "返回市场");
        Refresh();
    }
    void BuildDetail() {
        ClearContent();
        if (!detail) return;
        const auto& app = *detail;
        AddCover(content, covers[0], app.cover);
        Label(content, app.title, 0x6ddac8);
        Label(content, app.developer + "  /  " + app.category, 0x91a4bd);
        Label(content, app.id, 0x91a4bd);
        const char* runner = std::getenv("MICROPIXEL_RV32_RUNNER");
        const bool ready = std::filesystem::is_regular_file(runner ? runner : "build/pc-rv32/micropixel-rv32.elf");
        const bool available = Rv32Release() != nullptr;
        install_button = Button(content,
                                !available ? "暂无可用 RISC-V 版本"
                                : ready    ? "安装并运行（RV32）"
                                           : "需先构建 RV32 运行程序",
                                InstallClick, this);
        if (!ready || !available) lv_obj_add_state(install_button, LV_STATE_DISABLED);
        Label(content, "RISC-V 版本通过模拟器运行；实际性能和兼容性取决于应用。", 0xffce8c);
        Label(content, app.description.empty() ? app.summary : app.description);
        Label(content, "可用版本", 0x6ddac8);
        if (app.releases.empty()) Label(content, "暂无可用版本");
        for (const auto& release : app.releases) {
            Label(content, "v" + release.version + "  /  " + release.target + "  /  " +
                               std::to_string(release.size / 1024) + " KiB");
            if (!release.notes.empty()) Label(content, release.notes, 0x91a4bd);
        }
        SetStatus("官方详情已加载  /  " + std::to_string(app.releases.size()) + " 个可用版本");
    }
    void Update() {
        if (!root) return;
        if (auto result = worker.Poll()) {
            if (result->request.view == view) {
                if (result->request.kind == Kind::kCover) {
                    for (auto& cover : covers) {
                        if (cover.url != result->request.key || !cover.image || !cover.pixels.empty()) continue;
                        if (!result->error.empty()) {
                            lv_label_set_text(cover.placeholder, "封面不可用");
                            break;
                        }
                        cover.pixels = std::move(result->pixels);
                        auto& d = cover.descriptor;
                        d.header.magic = LV_IMAGE_HEADER_MAGIC;
                        d.header.cf = LV_COLOR_FORMAT_ARGB8888;
                        d.header.w = kCoverWidth;
                        d.header.h = kCoverHeight;
                        d.header.stride = kCoverWidth * 4;
                        d.data_size = cover.pixels.size();
                        d.data = cover.pixels.data();
                        lv_image_set_src(cover.image, &d);
                        lv_obj_set_hidden(cover.placeholder, true);
                        std::fprintf(stderr, "Market cover loaded\n");
                        break;
                    }
                } else if (!result->error.empty()) {
                    if (install_button) lv_obj_remove_state(install_button, LV_STATE_DISABLED);
                    SetStatus("市场加载失败，点击刷新重试：" + result->error, true);
                    Label(content, "请检查网络连接后重试。也可返回本地应用继续使用。", 0xffce8c);
                    std::fprintf(stderr, "Market error: %s\n", result->error.c_str());
                } else if (result->request.kind == Kind::kInstall) {
                    launch_path = std::move(result->installed);
                    SetStatus("安装完成，正在启动...");
                } else if (result->request.kind == Kind::kCatalog) {
                    apps = std::move(result->apps);
                    loaded = true;
                    std::fprintf(stderr, "Market catalog: %zu entries\n", apps.size());
                    BuildList();
                } else {
                    detail = std::move(result->detail);
                    std::fprintf(stderr, "Market detail: %s; %zu releases\n", detail->id.c_str(),
                                 detail->releases.size());
                    BuildDetail();
                }
            }
        }
        if (worker.Busy()) return;
        for (auto& cover : covers) {
            if (cover.attempted || cover.url.empty() || !cover.image) continue;
            cover.attempted = true;
            worker.Submit(Kind::kCover, cover.url, cover.url, view);
            break;
        }
    }
};
Market::Market() : state_(std::make_unique<State>()) {}
Market::~Market() = default;
void Market::Show(const std::string& id) { state_->Show(id); }
void Market::Hide() { state_->Hide(); }
void Market::Update() { state_->Update(); }
void Market::Back() { state_->Back(); }
bool Market::Visible() const { return state_->root != nullptr; }
bool Market::TakeHallRequest() { return std::exchange(state_->hall_requested, false); }
std::filesystem::path Market::TakeLaunchRequest() { return std::exchange(state_->launch_path, {}); }
void Market::ReportLaunchError(const std::string& error) {
    state_->SetStatus(error, true);
    if (state_->install_button) lv_obj_remove_state(state_->install_button, LV_STATE_DISABLED);
}
}  // namespace micropixel::pc
