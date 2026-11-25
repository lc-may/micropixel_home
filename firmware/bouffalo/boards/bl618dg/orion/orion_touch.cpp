// SPDX-License-Identifier: Apache-2.0
#include "orion_touch.hpp"

#include <array>
#include <cstdio>

#include "bflb_gpio.h"
#include "bflb_i2c.h"
#include "freertos/task.h"
#include "orion_pins.h"

extern "C" {
#include "board_gpio.h"
}

namespace micropixel::bouffalo {
namespace {

// ST7123 TDDI report page. Use the SDK hardware I2C path with an explicit STOP
// between the register pointer and read, as verified on Orion.
constexpr uint16_t kStatusReg = 0x0001;
constexpr uint16_t kResolutionReg = 0x0005;
constexpr uint16_t kReportReg = 0x0010;
constexpr uint16_t kMiscReg = 0x00f0;
constexpr uint16_t kIdReg = 0x00f4;
constexpr uint8_t kReportHasCoordinates = 0x08;
constexpr uint8_t kReportReset = 0x80;
constexpr uint8_t kMiscChecksum = 0x10;
constexpr uint8_t kPointValid = 0x80;
constexpr uint32_t kHeaderBytes = 4U;
constexpr uint32_t kPointBytes = 7U;
constexpr uint32_t kMaxSlots = 10U;

class I2cLock final {
   public:
    I2cLock() : acquired_(board_shared_i2c_acquire(true) == 0) {}
    ~I2cLock() {
        if (acquired_) board_shared_i2c_release();
    }
    explicit operator bool() const { return acquired_; }

   private:
    bool acquired_;
};

bool Read(uint16_t reg, uint8_t* bytes, uint16_t count) {
    auto* i2c = bflb_device_get_by_name("i2c0");
    uint8_t address[] = {static_cast<uint8_t>(reg >> 8U), static_cast<uint8_t>(reg)};
    bflb_i2c_msg_s pointer{.addr = ORION_TOUCH_ADDRESS, .flags = I2C_M_WRITE, .buffer = address, .length = 2U};
    bflb_i2c_msg_s reply{.addr = ORION_TOUCH_ADDRESS, .flags = I2C_M_READ, .buffer = bytes, .length = count};
    return bflb_i2c_transfer(i2c, &pointer, 1) == 0 && bflb_i2c_transfer(i2c, &reply, 1) == 0;
}

uint16_t Coordinate(const uint8_t* bytes) { return static_cast<uint16_t>((bytes[0] & 0x3fU) << 8U) | bytes[1]; }

bool Checksum(const uint8_t* bytes, uint32_t length) {
    uint8_t value = 0x5aU;
    for (uint32_t i = 0; i + 1U < length; ++i) {
        value += bytes[i];
        value = (value << 1U) | (value >> 7U);
    }
    return value == bytes[length - 1U];
}

}  // namespace

bool OrionTouch::Initialize(uint16_t width, uint16_t height) {
    auto* gpio = bflb_device_get_by_name("gpio");
    if (gpio == nullptr || board_shared_i2c_init() != 0) return false;
    // The LCD driver has released TP_RESET. Repeat the Timebox reset pulse
    // after the panel's DSI initialization.
    bflb_gpio_init(gpio, ORION_TOUCH_RESET_PIN, GPIO_OUTPUT | GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_reset(gpio, ORION_TOUCH_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20));
    bflb_gpio_set(gpio, ORION_TOUCH_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(100));
    bflb_gpio_init(gpio, ORION_TOUCH_INT_PIN, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN);

    const I2cLock lock;
    uint8_t status{};
    uint8_t id{};
    uint8_t misc{};
    uint8_t geometry[5]{};
    if (!lock || !Read(kStatusReg, &status, 1U) || !Read(kIdReg, &id, 1U) ||
        !Read(kResolutionReg, geometry, sizeof(geometry)) || !Read(kMiscReg, &misc, 1U)) {
        return false;
    }
    if (((status & 0x0fU) != 0U && (status & 0x0fU) != 4U) || Coordinate(geometry) != width ||
        Coordinate(geometry + 2) != height || geometry[4] == 0U || geometry[4] > kMaxSlots) {
        return false;
    }
    width_ = width;
    height_ = height;
    slots_ = geometry[4];
    checksum_ = (misc & kMiscChecksum) != 0U;
    std::printf("Orion ST7123 ready: %u x %u, slots=%u, ID=0x%02x\n", width_, height_, slots_, id);
    return true;
}

OrionTouchPoint OrionTouch::Poll() {
    if (slots_ == 0U) return {};
    const I2cLock lock;
    uint8_t header{};
    if (!lock || !Read(kReportReg, &header, 1U) || (header & kReportReset) != 0U) {
        if (++read_failures_ >= 3U) {
            point_.pressed = false;
            read_failures_ = 0U;
        }
        return point_;
    }
    if ((header & kReportHasCoordinates) == 0U) {
        read_failures_ = 0U;
        return point_;
    }

    std::array<uint8_t, kHeaderBytes + kMaxSlots * kPointBytes + 1U> report{};
    const uint32_t length = kHeaderBytes + slots_ * kPointBytes + (checksum_ ? 1U : 0U);
    if (!Read(kReportReg, report.data(), length) || (report[0] & kReportReset) != 0U ||
        (checksum_ && !Checksum(report.data(), length))) {
        if (++read_failures_ >= 3U) {
            point_.pressed = false;
            read_failures_ = 0U;
        }
        return point_;
    }
    read_failures_ = 0U;
    if ((report[0] & kReportHasCoordinates) == 0U) return point_;
    for (uint8_t slot = 0; slot < slots_; ++slot) {
        const uint8_t* item = report.data() + kHeaderBytes + slot * kPointBytes;
        if ((item[0] & kPointValid) == 0U) continue;
        const auto x = Coordinate(item);
        const auto y = Coordinate(item + 2);
        if (x < width_ && y < height_) {
            point_ = {x, y, true};
            return point_;
        }
    }
    point_.pressed = false;
    return point_;
}

}  // namespace micropixel::bouffalo
