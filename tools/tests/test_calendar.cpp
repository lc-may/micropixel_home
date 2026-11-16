// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <ctime>

#include "sdk/calendar.hpp"
using namespace micropixel::calendar;
int main() {
    assert(Leap(2000) && !Leap(2100) && DaysInMonth(2024, 2) == 29 && DaysInMonth(2026, 2) == 28);
    assert(!Valid({2026, 2, 29}) && !Valid({2026, 0, 1}) && !Valid({2026, 4, 31}));
    assert(Days({1970, 1, 1}) == 0 && Weekday({1970, 1, 1}) == 3);
    for (int64_t day = 0; day < 48000; ++day) {
        auto date = FromDays(day);
        assert(Valid(date) && Days(date) == day);
        std::time_t t = day * 86400;
        std::tm reference{};
        gmtime_r(&t, &reference);
        assert(date.year == reference.tm_year + 1900 && date.month == uint32_t(reference.tm_mon + 1) &&
               date.day == uint32_t(reference.tm_mday));
        assert(Weekday(date) == uint32_t((reference.tm_wday + 6) % 7));
    }
    assert((LocalDate(Utc({2026, 12, 31}, 23 * 60 + 59, 480), 480) == Date{2026, 12, 31}));
    assert((LocalDate(Utc({2026, 12, 31}, 23 * 60 + 59, 480) + 60, 480) == Date{2027, 1, 1}));
    assert((FromDays(Days({2024, 2, 28}) + 1) == Date{2024, 2, 29}));
    std::puts(
        "PASS Gregorian calendar against OS dates: 48000 days, leap centuries, month lengths, weekdays, UTC offsets "
        "and year rollover");
}
