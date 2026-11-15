// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>

#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
using Clock = std::chrono::steady_clock;
const auto origin = Clock::now();
std::recursive_timed_mutex lvgl_mutex;
std::array<PcTask, 32> tasks;
std::mutex tasks_mutex;
thread_local TaskHandle_t current_task{};

template <typename Predicate>
bool Wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, TickType_t timeout, Predicate predicate) {
    if (timeout == portMAX_DELAY) {
        cv.wait(lock, predicate);
        return true;
    }
    return cv.wait_for(lock, std::chrono::milliseconds(timeout), predicate);
}
BaseType_t Send(QueueHandle_t q, const void* item, TickType_t timeout, bool front) {
    if (!q || !item) return pdFALSE;
    std::unique_lock lock(q->mutex);
    if (!Wait(q->space, lock, timeout, [q] { return q->count < q->capacity; })) return pdFALSE;
    if (front) q->head = (q->head + q->capacity - 1) % q->capacity;
    const size_t index = front ? q->head : (q->head + q->count) % q->capacity;
    std::memcpy(q->bytes + index * q->item_size, item, q->item_size);
    ++q->count;
    lock.unlock();
    q->ready.notify_one();
    return pdTRUE;
}
BaseType_t Receive(QueueHandle_t q, void* item, TickType_t timeout, bool peek) {
    if (!q || !item) return pdFALSE;
    std::unique_lock lock(q->mutex);
    if (!Wait(q->ready, lock, timeout, [q] { return q->count != 0; })) return pdFALSE;
    std::memcpy(item, q->bytes + q->head * q->item_size, q->item_size);
    if (!peek) {
        q->head = (q->head + 1) % q->capacity;
        --q->count;
    }
    lock.unlock();
    q->space.notify_one();
    return pdTRUE;
}
}  // namespace

extern "C" const char* esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK:
            return "OK";
        case ESP_ERR_NO_MEM:
            return "out of memory";
        case ESP_ERR_INVALID_ARG:
            return "invalid argument";
        case ESP_ERR_INVALID_STATE:
            return "invalid state";
        case ESP_ERR_NOT_FOUND:
            return "not found";
        case ESP_ERR_NOT_SUPPORTED:
            return "unsupported";
        case ESP_ERR_TIMEOUT:
            return "timeout";
        default:
            return "platform error";
    }
}
extern "C" esp_err_t esp_lv_adapter_lock(int timeout) {
    if (timeout < 0) {
        lvgl_mutex.lock();
        return ESP_OK;
    }
    return lvgl_mutex.try_lock_for(std::chrono::milliseconds(timeout)) ? ESP_OK : ESP_ERR_TIMEOUT;
}
extern "C" void esp_lv_adapter_unlock() { lvgl_mutex.unlock(); }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void* ctx, UBaseType_t, TaskHandle_t* out,
                                   BaseType_t) {
    if (!fn || !out) return pdFALSE;
    std::lock_guard lock(tasks_mutex);
    PcTask* task = nullptr;
    for (auto& candidate : tasks)
        if (candidate.finished.load(std::memory_order_acquire)) {
            task = &candidate;
            break;
        }
    if (!task) return pdFALSE;
    if (task->worker.joinable()) task->worker.join();
    task->notifications = 0;
    task->finished = false;
    *out = task;
    task->worker = std::thread([task, fn, ctx] {
        current_task = task;
        fn(ctx);
        current_task = nullptr;
        task->finished.store(true, std::memory_order_release);
    });
    return pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* ctx, UBaseType_t p,
                       TaskHandle_t* out) {
    return xTaskCreatePinnedToCore(fn, name, stack, ctx, p, out, 0);
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
void vTaskDelete(TaskHandle_t task) {
    // Shared workers call this only at the tail of their entry point. Foreign
    // thread cancellation is deliberately unsupported; owners signal and join.
    if (task && task != current_task && task->worker.joinable()) task->worker.join();
}
void micropixel::pc::JoinTasks() {
    for (auto& task : tasks) {
        if (task.finished.load(std::memory_order_acquire) && task.worker.joinable()) task.worker.join();
    }
}
void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(esp_timer_get_time() / 1000); }
void xTaskNotifyGive(TaskHandle_t task) {
    if (!task) return;
    {
        std::lock_guard lock(task->mutex);
        ++task->notifications;
    }
    task->ready.notify_one();
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
    if (!current_task) return 0;
    auto* task = current_task;
    std::unique_lock lock(task->mutex);
    if (!Wait(task->ready, lock, timeout, [task] { return task->notifications != 0; })) return 0;
    auto count = task->notifications;
    if (clear)
        task->notifications = 0;
    else
        --task->notifications;
    return count;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s) {
    s->available = false;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) {
    s->available = true;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary() {
    auto* s = new (std::nothrow) StaticSemaphore_t;
    if (s) s->allocated = true;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* s = xSemaphoreCreateBinary();
    if (s) s->available = true;
    return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) {
    if (!s) return pdFALSE;
    std::unique_lock lock(s->mutex);
    if (!Wait(s->ready, lock, timeout, [s] { return s->available; })) return pdFALSE;
    s->available = false;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFALSE;
    {
        std::lock_guard lock(s->mutex);
        s->available = true;
    }
    s->ready.notify_one();
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s) {
    if (s && s->allocated) delete s;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t cap, UBaseType_t size, uint8_t* bytes, StaticQueue_t* q) {
    if (!cap || !size || !bytes || !q) return nullptr;
    q->bytes = bytes;
    q->capacity = cap;
    q->item_size = size;
    return q;
}
QueueHandle_t xQueueCreate(UBaseType_t cap, UBaseType_t size) {
    if (!cap || !size || size > SIZE_MAX / cap) return nullptr;
    std::unique_ptr<StaticQueue_t> q(new (std::nothrow) StaticQueue_t);
    if (!q) return nullptr;
    auto* bytes = new (std::nothrow) uint8_t[static_cast<size_t>(cap) * size];
    if (!bytes) return nullptr;
    xQueueCreateStatic(cap, size, bytes, q.get());
    q->allocated = true;
    return q.release();
}
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t t) { return Send(q, item, t, false); }
BaseType_t xQueueSendToFront(QueueHandle_t q, const void* item, TickType_t t) { return Send(q, item, t, true); }
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t t) { return Receive(q, item, t, false); }
BaseType_t xQueuePeek(QueueHandle_t q, void* item, TickType_t t) { return Receive(q, item, t, true); }
BaseType_t xQueueReset(QueueHandle_t q) {
    if (!q) return pdFALSE;
    {
        std::lock_guard lock(q->mutex);
        q->head = q->count = 0;
    }
    q->space.notify_all();
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void* item) {
    if (!q || q->capacity != 1 || !item) return pdFALSE;
    {
        std::lock_guard lock(q->mutex);
        std::memcpy(q->bytes, item, q->item_size);
        q->count = 1;
        q->head = 0;
    }
    q->ready.notify_one();
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    if (!q) return 0;
    std::lock_guard lock(q->mutex);
    return q->count;
}
void vQueueDelete(QueueHandle_t q) {
    if (q && q->allocated) {
        delete[] q->bytes;
        delete q;
    }
}

struct PcTimer {
    std::mutex mutex;
    std::condition_variable ready;
    std::thread worker;
    esp_timer_create_args_t args{};
    uint64_t deadline{}, period{}, generation{};
    bool active{}, stopping{}, in_callback{};
};
extern "C" int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin).count();
}
extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    if (!args || !args->callback || !out) return ESP_ERR_INVALID_ARG;
    std::unique_ptr<PcTimer> holder(new (std::nothrow) PcTimer);
    if (!holder) return ESP_ERR_NO_MEM;
    auto* t = holder.get();
    t->args = *args;
    t->worker = std::thread([t] {
        std::unique_lock lock(t->mutex);
        while (!t->stopping) {
            t->ready.wait(lock, [t] { return t->stopping || t->active; });
            if (t->stopping) break;
            const uint64_t generation = t->generation;
            const auto deadline = origin + std::chrono::microseconds(t->deadline);
            if (t->ready.wait_until(lock, deadline, [t, generation] {
                    return t->stopping || !t->active || generation != t->generation;
                }))
                continue;
            if (t->period)
                t->deadline = static_cast<uint64_t>(esp_timer_get_time()) + t->period;
            else
                t->active = false;
            t->in_callback = true;
            lock.unlock();
            t->args.callback(t->args.arg);
            lock.lock();
            t->in_callback = false;
            t->ready.notify_all();
        }
    });
    *out = holder.release();
    return ESP_OK;
}
static esp_err_t StartTimer(PcTimer* t, uint64_t period, uint64_t deadline) {
    if (!t || !deadline) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard lock(t->mutex);
        if (t->active) return ESP_ERR_INVALID_STATE;
        t->period = period;
        t->deadline = deadline;
        t->active = true;
        ++t->generation;
    }
    t->ready.notify_all();
    return ESP_OK;
}
extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    return StartTimer(t, 0, esp_timer_get_time() + us);
}
extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) {
    return us ? StartTimer(t, us, esp_timer_get_time() + us) : ESP_ERR_INVALID_ARG;
}
extern "C" esp_err_t esp_timer_start_once_at(esp_timer_handle_t t, uint64_t at) { return StartTimer(t, 0, at); }
extern "C" esp_err_t esp_timer_start_periodic_at(esp_timer_handle_t t, uint64_t us, uint64_t at) {
    return us ? StartTimer(t, us, at) : ESP_ERR_INVALID_ARG;
}
extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    if (!t) return ESP_ERR_INVALID_ARG;
    std::lock_guard lock(t->mutex);
    if (!t->active) return ESP_ERR_INVALID_STATE;
    t->active = false;
    ++t->generation;
    t->ready.notify_all();
    return ESP_OK;
}
extern "C" esp_err_t esp_timer_stop_blocking(esp_timer_handle_t t) {
    auto result = esp_timer_stop(t);
    if (!t) return result;
    std::unique_lock lock(t->mutex);
    t->ready.wait(lock, [t] { return !t->in_callback; });
    return result;
}
extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t t) {
    if (!t) return ESP_ERR_INVALID_ARG;
    {
        std::lock_guard lock(t->mutex);
        t->stopping = true;
    }
    t->ready.notify_all();
    t->worker.join();
    delete t;
    return ESP_OK;
}
extern "C" esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t t, uint64_t* out) {
    if (!t || !out) return ESP_ERR_INVALID_ARG;
    std::lock_guard lock(t->mutex);
    if (!t->active) return ESP_ERR_INVALID_STATE;
    *out = t->deadline;
    return ESP_OK;
}
extern "C" bool esp_timer_is_active(esp_timer_handle_t t) {
    if (!t) return false;
    std::lock_guard lock(t->mutex);
    return t->active;
}
extern "C" void micropixel_watchdog_checkpoint() {}
extern "C" void micropixel_watchdog_pause() {}
extern "C" void micropixel_watchdog_resume() {}
// The PC target intentionally loads portable Wasm, never flash-mapped AOT.
extern "C" bool wasm_runtime_is_xip_file(const uint8_t*, uint32_t) { return false; }
