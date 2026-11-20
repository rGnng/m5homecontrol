#include "ha_clock.h"

#include <cassert>
#include <cstdio>

static int64_t epoch(int year, int month, int day, int hour, int minute, int second = 0) {
    struct tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    return timegm(&value);
}

int main() {
    HaClock clock;
    struct tm local;
    assert(!clock.ready() && !clock.localTime(1000, local));
    int64_t utc = epoch(2026, 1, 1, 0, 0);
    assert(clock.sync(utc, 9 * 3600, 1000));
    assert(clock.localTime(62000, local));
    assert(local.tm_hour == 9 && local.tm_min == 1 && local.tm_sec == 1);
    assert(clock.sync(utc, -5 * 3600, 1000));
    assert(clock.localTime(1000, local));
    assert(local.tm_year == 125 && local.tm_mon == 11 && local.tm_mday == 31 && local.tm_hour == 19);
    assert(clock.sync(utc, 5 * 3600 + 45 * 60, 1000));
    assert(clock.localTime(1000, local) && local.tm_hour == 5 && local.tm_min == 45);
    assert(clock.sync(utc, 10 * 3600 + 30 * 60, 1000));
    assert(clock.localTime(1000, local) && local.tm_hour == 10 && local.tm_min == 30);

    // HA computes the offset; a new sample applies its DST change atomically.
    assert(clock.sync(epoch(2026, 3, 8, 6, 59, 59), -5 * 3600, 1000));
    assert(clock.localTime(1000, local) && local.tm_hour == 1 && local.tm_min == 59);
    assert(clock.sync(epoch(2026, 3, 8, 7, 0), -4 * 3600, 2000));
    assert(clock.localTime(2000, local) && local.tm_hour == 3 && local.tm_min == 0);
    assert(clock.sync(epoch(2026, 11, 1, 5, 59), -4 * 3600, 3000));
    assert(clock.localTime(3000, local) && local.tm_hour == 1 && local.tm_min == 59);
    assert(clock.sync(epoch(2026, 11, 1, 6, 0), -5 * 3600, 63000));
    assert(clock.localTime(63000, local) && local.tm_hour == 1 && local.tm_min == 0);

    uint64_t beforeWrap = (1ULL << 32) - 5000;
    assert(clock.sync(utc, 0, beforeWrap));
    assert(clock.utcEpoch(beforeWrap + 10000) == utc + 10);
    assert(clock.utcEpoch(beforeWrap + 60ULL * 86400 * 1000) == utc + 60LL * 86400);
    assert(!clock.sync(0, 0, 1));
    assert(!clock.sync(utc, 15 * 3600, 1));
    assert(clock.offsetSeconds() == 0 && clock.utcEpoch(beforeWrap) == utc);
    std::puts("PASS: HA clock, signed/fractional offsets, date rollover, DST, long offline advance, 32-bit millis rollover, invalid samples");
}
