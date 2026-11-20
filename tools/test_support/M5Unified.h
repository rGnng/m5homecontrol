#pragma once

#include <cassert>
#include <cstdint>

// Host substitutes for the hardware API used by src/power_status.cpp.
namespace m5 {
enum class board_t { board_M5Stack };
struct Power_Class {
    enum is_charging_t { is_discharging, is_charging, charge_unknown };
};
}

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }

struct Pmic {
    bool online = false, wakeOnBegin = false;
    unsigned reads = 0, begins = 0, writes = 0;
    uint8_t control = 0xb1, handoff = 0x99, input = 0, full = 0, level = 0;

    void setClock(unsigned) {}
    bool readRegister(uint8_t reg, uint8_t* value, unsigned) {
        ++reads;
        if (!online) return false;
        switch (reg) {
            case 0x00: *value = control; break;
            case 0x01: *value = handoff; break;
            case 0x70: *value = input; break;
            case 0x71: *value = full; break;
            case 0x78: *value = level; break;
            default: return false;
        }
        return true;
    }
    bool begin() {
        ++begins;
        if (wakeOnBegin) online = true;
        return online;
    }
    bool writeRegister8(uint8_t reg, uint8_t value) {
        ++writes;
        // The power manager must never alter charger voltage/current registers.
        assert(reg == 0x00 || reg == 0x01);
        if (reg == 0x00) control = value;
        else handoff = value;
        return true;
    }
};

struct FakePower {
    Pmic Ip5306;
    int getBatteryLevel() { return -1; }
    int getBatteryVoltage() { return 0; }
    int getVBUSVoltage() { return -1; }
    m5::Power_Class::is_charging_t isCharging() { return m5::Power_Class::charge_unknown; }
};
struct Bus {
    unsigned begins = 0;
    bool begin() { ++begins; return true; }
};
struct Device {
    FakePower Power;
    Bus In_I2C;
    m5::board_t getBoard() { return m5::board_t::board_M5Stack; }
};
extern Device M5;
struct Logger { template<class... T> void printf(const char*, T...) {} };
extern Logger Serial;
