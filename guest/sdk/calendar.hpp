// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "sdk/fixed_string.hpp"
namespace micropixel::calendar {
struct Date {
    int32_t year{1970};
    uint32_t month{1}, day{1};
    friend constexpr bool operator==(Date, Date) = default;
};
constexpr bool Leap(int32_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
constexpr uint32_t DaysInMonth(int32_t year, uint32_t month) {
    constexpr uint32_t lengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 ? lengths[month - 1] + (month == 2 && Leap(year)) : 0;
}
constexpr bool Valid(Date date) {
    return date.year >= 1970 && date.year <= 9999 && date.day && date.day <= DaysInMonth(date.year, date.month);
}
constexpr int64_t Days(Date date) {
    if (!Valid(date)) return -1;
    int64_t n =
        int64_t(date.year - 1970) * 365 + (date.year - 1) / 4 - (date.year - 1) / 100 + (date.year - 1) / 400 - 477;
    for (uint32_t m = 1; m < date.month; ++m) n += DaysInMonth(date.year, m);
    return n + date.day - 1;
}
constexpr Date FromDays(int64_t n) {
    if (n < 0 || n > 2932896) return {};
    int32_t low = 1970, high = 10000;
    while (low + 1 < high) {
        const auto mid = (low + high) / 2;
        if (Days({mid, 1, 1}) <= n)
            low = mid;
        else
            high = mid;
    }
    Date date{low, 1, 1};
    n -= Days(date);
    while (n >= DaysInMonth(date.year, date.month)) {
        n -= DaysInMonth(date.year, date.month);
        ++date.month;
    }
    date.day = static_cast<uint32_t>(n) + 1;
    return date;
}
constexpr int64_t LocalSeconds(uint64_t utc, int32_t offset) {
    return static_cast<int64_t>(utc) + int64_t(offset) * 60;
}
constexpr Date LocalDate(uint64_t utc, int32_t offset) { return FromDays(LocalSeconds(utc, offset) / 86400); }
constexpr uint32_t Weekday(Date date) { return static_cast<uint32_t>((Days(date) + 3) % 7); }  // Monday = 0.
constexpr int64_t Utc(Date date, uint32_t minute, int32_t offset) {
    return Valid(date) && minute < 1440 ? Days(date) * 86400 + int64_t(minute) * 60 - int64_t(offset) * 60 : -1;
}
inline FixedString<32> FormatDate(Date date) {
    FixedString<32> text;
    text.AppendInt(date.year);
    text.Append("年");
    text.AppendUint(date.month);
    text.Append("月");
    text.AppendUint(date.day);
    text.Append("日");
    return text;
}
inline FixedString<16> FormatTime(uint32_t minute) {
    FixedString<16> text;
    if (minute / 60 < 10) text.Append("0");
    text.AppendUint(minute / 60);
    text.Append(":");
    if (minute % 60 < 10) text.Append("0");
    text.AppendUint(minute % 60);
    return text;
}
inline const char* WeekdayName(Date date) {
    constexpr const char* days[]{"星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"};
    return days[Weekday(date)];
}
}  // namespace micropixel::calendar
