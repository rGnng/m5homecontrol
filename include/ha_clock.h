#pragma once

#include <cstdint>
#include <ctime>
#include <limits>

// HA supplies UTC plus its current timezone offset. Between updates, advance
// with a 64-bit monotonic clock, independent of ESP32's POSIX timezone support.
class HaClock {
public:
    bool sync(int64_t epoch, int32_t offset, uint64_t nowMs) {
        if (epoch < 1577836800LL || epoch > 4102444800LL ||
            offset < -14 * 3600 || offset > 14 * 3600) return false;
        epoch_ = epoch;
        offset_ = offset;
        syncedAt_ = nowMs;
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }
    int32_t offsetSeconds() const { return offset_; }
    uint64_t ageMs(uint64_t nowMs) const { return nowMs >= syncedAt_ ? nowMs - syncedAt_ : 0; }
    int64_t utcEpoch(uint64_t nowMs) const { return epoch_ + ageMs(nowMs) / 1000; }

    bool localTime(uint64_t nowMs, struct tm& out) const {
        if (!ready_) return false;
        int64_t local = utcEpoch(nowMs) + offset_;
        if (local > std::numeric_limits<time_t>::max()) return false;
        time_t value = static_cast<time_t>(local);
        return gmtime_r(&value, &out) != nullptr;
    }

private:
    int64_t epoch_ = 0;
    int32_t offset_ = 0;
    uint64_t syncedAt_ = 0;
    bool ready_ = false;
};
