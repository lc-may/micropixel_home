// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

#include "rv32/protocol.h"
#include "session.hpp"
#include "work/background_executor.hpp"

extern char** environ;
namespace micropixel::pc {
namespace {
using Clock = std::chrono::steady_clock;
struct Machine {
    pid_t pid = -1;
    int socket = -1, ram_fd = -1;
    uint8_t* ram{};
    std::string directory;
    std::optional<Clock::time_point> stopping;
    ~Machine() {
        if (socket >= 0) close(socket);
        if (pid > 0) {
            kill(pid, SIGTERM);
            for (unsigned i = 0; i < 50; ++i) {
                if (waitpid(pid, nullptr, WNOHANG) == pid) {
                    pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pid > 0) {
                kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
        }
        if (ram) munmap(ram, MP_RV_RAM_SIZE);
        if (ram_fd >= 0) close(ram_fd);
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
    bool Start(const PortableApp& app, std::atomic<bool>& stop) {
        char path[] = "/tmp/micropixel-rv32-XXXXXX";
        if (!mkdtemp(path)) return false;
        directory = path;
        const auto ram_path = directory + "/ram";
        const auto socket_path = directory + "/rpc";
        ram_fd = open(ram_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (ram_fd < 0 || ftruncate(ram_fd, MP_RV_RAM_SIZE) != 0) return false;
        void* mapping = mmap(nullptr, MP_RV_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ram_fd, 0);
        if (mapping == MAP_FAILED) return false;
        ram = static_cast<uint8_t*>(mapping);
        const mp_rv_boot boot{MP_RV_MAGIC, MP_RV_VERSION, static_cast<uint32_t>(app.wasm.size()),
                              app.package.aot_flags};
        std::memcpy(ram + MP_RV_BOOT_ADDRESS - MP_RV_RAM_BASE, &boot, sizeof(boot));
        std::memcpy(ram + MP_RV_AOT_ADDRESS - MP_RV_RAM_BASE, app.wasm.data(), app.wasm.size());
        const char* emulator = std::getenv("MICROPIXEL_QEMU_RV32");
        if (!emulator) emulator = "qemu-system-riscv32";
        const char* firmware = std::getenv("MICROPIXEL_RV32_RUNNER");
        if (!firmware) firmware = "build/pc-rv32/micropixel-rv32.elf";
        if (!std::filesystem::is_regular_file(firmware)) {
            std::fprintf(stderr, "RV32 runner missing: run bash tools/pc.sh rv32-build\n");
            return false;
        }
        const std::string memory = "memory-backend-file,id=ram,size=134217728,share=on,mem-path=" + ram_path;
        const std::string channel = "socket,id=rpc,path=" + socket_path + ",server=on,wait=on";
        std::array<const char*, 24> arguments{emulator,   "-machine",     "virt,memory-backend=ram",
                                              "-cpu",     "rv32",         "-smp",
                                              "1",        "-m",           "128M",
                                              "-object",  memory.c_str(), "-bios",
                                              "none",     "-kernel",      firmware,
                                              "-display", "none",         "-monitor",
                                              "none",     "-chardev",     channel.c_str(),
                                              "-serial",  "chardev:rpc",  nullptr};
        const int status =
            posix_spawnp(&pid, emulator, nullptr, nullptr, const_cast<char**>(arguments.data()), environ);
        if (status) {
            pid = -1;
            std::fprintf(stderr, "QEMU spawn: %s\n", std::strerror(status));
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!stop && Clock::now() < deadline) {
            socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (socket < 0) return false;
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::strcpy(address.sun_path, socket_path.c_str());
            if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return true;
            close(socket);
            socket = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    bool Transfer(void* bytes, size_t size, bool write, std::atomic<bool>& stop) {
        auto* p = static_cast<uint8_t*>(bytes);
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (size && Clock::now() < deadline) {
            if (stop) {
                if (!stopping) stopping = Clock::now();
                if (Clock::now() - *stopping > std::chrono::milliseconds(400)) return false;
            }
            const auto count = write ? send(socket, p, size, MSG_NOSIGNAL) : recv(socket, p, size, 0);
            if (count > 0) {
                p += count;
                size -= count;
                continue;
            }
            if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
            pollfd event{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
            poll(&event, 1, 25);
        }
        return size == 0;
    }
};
struct Memory {
    uint8_t* ram{};
    uint32_t base{}, size{};
    bool Set(const mp_rv_frame& frame) {
        base = frame.memory_base;
        size = frame.memory_size;
        return base >= MP_RV_RAM_BASE && base < MP_RV_BOOT_ADDRESS && size > 0 && size <= MP_RV_MEMORY_LIMIT &&
               size <= MP_RV_BOOT_ADDRESS - base;
    }
    uint8_t* Address(uint32_t physical, uint32_t length) const {
        if (physical < base || physical - base > size || length > size - (physical - base)) return nullptr;
        return ram + physical - MP_RV_RAM_BASE;
    }
    static bool Resolve(void* context, uint32_t offset, uint32_t length, uint8_t** output) {
        auto& memory = *static_cast<Memory*>(context);
        if (!output || !length || offset >= memory.size || length > memory.size - offset) return false;
        *output = memory.ram + memory.base - MP_RV_RAM_BASE + offset;
        return true;
    }
};
int64_t Dispatch(runtime::GuestContext& context, Memory& memory, const mp_rv_frame& frame) {
    const auto* a = frame.args;
    if (frame.kind == MP_RV_ABI) return MICROPIXEL_ABI_VERSION;
    if (frame.kind == MP_RV_CLOCK) return context.AppTimeNow();
    if (!memory.Set(frame)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    auto pointer = [&](unsigned index, uint32_t size) { return memory.Address(a[index], size); };
    switch (frame.kind) {
        case MP_RV_LOG: {
            auto* bytes = pointer(1, a[2]);
            if ((!bytes && a[2]) || a[2] > MICROPIXEL_ABI_MAX_LOG_BYTES || a[0] < MICROPIXEL_LOG_DEBUG ||
                a[0] > MICROPIXEL_LOG_ERROR)
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            context.WriteLog(a[0], bytes, a[2]);
            return MICROPIXEL_STATUS_OK;
        }
        case MP_RV_EVENT: {
            auto* bytes = pointer(0, a[1]);
            if (!bytes || a[1] < sizeof(micropixel_event_t)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            micropixel_event_t event{};
            auto result = context.WaitEvent(event, uint64_t(a[2]) | uint64_t(a[3]) << 32);
            if (result == runtime::EventWaitResult::kClosed) return MICROPIXEL_STATUS_CLOSED;
            if (result == runtime::EventWaitResult::kTimeout) return MICROPIXEL_STATUS_TIMEOUT;
            std::memcpy(bytes, &event, sizeof(event));
            context.NoteEventDelivered(event);
            return MICROPIXEL_STATUS_OK;
        }
        case MP_RV_OPEN: {
            auto* bytes = pointer(2, a[3]);
            if (!bytes || a[3] < sizeof(micropixel_service_info_t)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            micropixel_service_info_t info{};
            auto result = context.ServiceOpen(a[0], a[1], info, a[3]);
            std::memcpy(bytes, &info, sizeof(info));
            return result;
        }
        case MP_RV_CALL: {
            auto* request = pointer(2, a[3]);
            auto* response = pointer(4, a[5]);
            auto* size_pointer = pointer(6, sizeof(uint32_t));
            if ((!request && a[3]) || (!response && a[5]) || !size_pointer) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            uint32_t size = 0;
            auto result = context.ServiceCall(a[0], a[1], request, a[3], response, a[5], size);
            std::memcpy(size_pointer, &size, sizeof(size));
            return result;
        }
        case MP_RV_SUBMIT: {
            auto* bytes = pointer(2, a[3]);
            if (!bytes || !a[3]) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return context.ServiceSubmit(a[0], a[1], bytes, a[3]);
        }
        default:
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
}
}  // namespace
void Session::RunRv32() {
    static_assert(std::endian::native == std::endian::little);
    Machine machine;
    if (!machine.Start(app_, stop_requested_)) {
        std::fprintf(stderr, "Cannot start RV32 machine\n");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        child_pid_ = machine.pid;
    }
    work::BackgroundExecutor background;
    auto arguments = launch_arguments_;
    arguments.size = sizeof(arguments);
    Memory memory{machine.ram};
    runtime::GuestContext context(app_.package, devices_, background, "en", arguments, this);
    context.BindGuestMemory({.context = &memory, .resolve = Memory::Resolve, .stable_base = false});
    {
        std::lock_guard lock(mutex_);
        context_ = &context;
        if (stop_requested_) (void)context.RequestStop();
    }
    if (context.valid()) {
        uint32_t last_sequence = 0;
        bool hello = false;
        for (;;) {
            mp_rv_frame frame{};
            if (!machine.Transfer(&frame, sizeof(frame), false, stop_requested_)) break;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (frame.magic != MP_RV_MAGIC || frame.version != MP_RV_VERSION || frame.reserved) break;
            if (frame.kind == MP_RV_TRACE) {
                std::array<char, MP_RV_TRACE_LIMIT> text{};
                if (frame.payload_size > text.size() ||
                    !machine.Transfer(text.data(), frame.payload_size, false, stop_requested_))
                    break;
                std::fprintf(stderr, "[RV32] %.*s", static_cast<int>(frame.payload_size), text.data());
                continue;
            }
            if (frame.payload_size) break;
            if (frame.kind == MP_RV_DONE) {
                result_ = frame.result_lo <= 255 ? frame.result_lo : 1;
                std::fprintf(stderr, "Guest completed: result=%d (RV32 AOT)\n", result_);
                break;
            }
            if (frame.sequence != last_sequence + 1) break;
            last_sequence = frame.sequence;
            int64_t result = 0;
            if (frame.kind == MP_RV_HELLO && !hello) {
                hello = true;
                std::fprintf(stderr, "RV32 bridge connected: %s\n", app_.app_id.c_str());
            } else if (!hello || frame.kind == MP_RV_HELLO) {
                break;
            } else {
                result = Dispatch(context, memory, frame);
            }
            mp_rv_frame reply{};
            reply.magic = MP_RV_MAGIC;
            reply.version = MP_RV_VERSION;
            reply.kind = MP_RV_REPLY;
            reply.sequence = frame.sequence;
            reply.result_lo = static_cast<uint32_t>(result);
            reply.result_hi = static_cast<uint64_t>(result) >> 32;
            std::atomic_thread_fence(std::memory_order_release);
            if (!machine.Transfer(&reply, sizeof(reply), true, stop_requested_)) break;
        }
    }
    if (stop_requested_)
        result_ = 0;
    else if (result_ != 0)
        std::fprintf(stderr, "RV32 session failed: %s\n", context.LastPanic());
    context.ForceStop();
    {
        std::lock_guard lock(mutex_);
        context_ = nullptr;
        child_pid_ = 0;
    }
}
}  // namespace micropixel::pc
