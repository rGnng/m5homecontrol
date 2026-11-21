// =====================================================================
//  Home Control - Home Assistant remote for M5Stack Core Basic v2.7
//  Eight-row menu, live state via Home Assistant WebSocket API.
// =====================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <utility/led/LED_Strip_Class.hpp>

#include "app_config.h"
#include "connected_power.h"
#include "backlight_schedule.h"
#include "power_status.h"
#include "side_led_bus.h"
#include "ha_client.h"
#include "keypad_controls.h"
#include "sfx_data.h"
#include "ui_font.h"
#include "ui_font_data.h"
#include "utf8_text.h"

// ---------------------------------------------------------------------
//  Theme
// ---------------------------------------------------------------------
static constexpr uint16_t C_BAR      = 0x39A7;
static constexpr uint16_t C_BG       = TFT_BLACK;
static constexpr uint16_t C_CARD     = 0x18E3;
static constexpr uint16_t C_CARD_SEL = 0x10A2;
static constexpr uint16_t C_LINE     = 0x3186;
static constexpr uint16_t C_DIVIDER  = 0x4208;
static constexpr uint16_t C_TEXT     = TFT_WHITE;
static constexpr uint16_t C_DIM      = TFT_DARKGREY;
static constexpr uint16_t C_OFF      = 0x632C;
static constexpr uint16_t C_GREEN    = TFT_GREEN;
static constexpr uint16_t C_YELLOW   = TFT_YELLOW;
static constexpr uint16_t C_CYAN     = TFT_CYAN;
static constexpr uint16_t C_MAGENTA  = TFT_MAGENTA;
static constexpr uint16_t C_RED      = TFT_RED;
static constexpr uint16_t C_ORANGE   = TFT_ORANGE;

// Flipper palette
static constexpr uint16_t F_BAR     = 0x28A1;
static constexpr uint16_t F_ORANGE  = 0xE4C7;
static constexpr uint16_t F_CREAM   = 0xF739;
static constexpr uint16_t F_SELECTED = 0x82E6;
static constexpr uint16_t F_DARK    = 0x1860;
static constexpr uint16_t F_LINE    = 0x8A62;
static constexpr uint16_t F_DIM     = 0x6A28;
static constexpr uint16_t F_ICON    = 0xF739;
static constexpr uint16_t F_TRACK   = 0xC3C5;

struct ThemeColors {
    uint16_t bg, barBg, barText, dotOn, dotOff;
    uint16_t rowBg, rowSel, rowText, rowSelText;
    uint16_t numBg, numText, numBgEmpty, numTextEmpty;
    uint16_t numSelBg, numSelText, numberLine, divider;
    uint16_t cardBg, cardLine, text, dim, off;
    uint16_t accentLight, accentSwitch, accentClimate, accentScene;
    uint16_t accentMedia, accentSensor, accentCover;
    uint16_t iconWell, barTrack;
    uint16_t editBg, editText;
    uint16_t commandLedColor;
    bool dotted;
};

static ThemeColors g_theme;
static bool g_dimTheme = false;

static void applyTheme() {
    if (g_config.theme == 1) {
        g_theme = {
            C_BG, C_BAR, TFT_LIGHTGREY, TFT_WHITE, C_DIM,
            TFT_BLACK, TFT_NAVY, TFT_WHITE, C_GREEN,
            0x2104, TFT_WHITE, C_BAR, TFT_BLACK,
            C_GREEN, TFT_BLACK, 0, C_DIVIDER,
            C_CARD, C_LINE, TFT_WHITE, C_DIM, C_OFF,
            C_YELLOW, C_GREEN, C_CYAN, C_GREEN,
            C_MAGENTA, C_ORANGE, C_CYAN,
            0x2104, 0x2945,
            TFT_MAROON, C_YELLOW,
            C_GREEN,
            false
        };
    } else {
        g_theme = {
            F_CREAM, F_BAR, F_CREAM, F_CREAM, F_LINE,
            F_ORANGE, F_SELECTED, F_DARK, F_CREAM,
            F_ORANGE, F_DARK, F_ORANGE, F_DARK,
            F_SELECTED, F_CREAM, F_LINE, F_LINE,
            F_ORANGE, F_LINE, F_DARK, F_DIM, F_DIM,
            F_DARK, F_DARK, F_DARK, F_DARK,
            F_DARK, F_DARK, F_DARK,
            F_ICON, F_TRACK,
            F_DARK, F_CREAM,
            F_ORANGE,
            true
        };
    }
    if (g_dimTheme) {
        // An intentionally faint monochrome palette, independent of the
        // selected daytime theme. Keep the theme's command LED hue.
        constexpr uint16_t text = 0x3186, faint = 0x18C3, selected = 0x4208;
        g_theme.bg = g_theme.barBg = g_theme.rowBg = TFT_BLACK;
        g_theme.numBg = g_theme.numBgEmpty = g_theme.cardBg = TFT_BLACK;
        g_theme.rowSel = g_theme.numSelBg = 0x0841;
        g_theme.rowText = g_theme.numText = g_theme.text = text;
        g_theme.rowSelText = g_theme.numSelText = selected;
        g_theme.barText = g_theme.dotOn = text;
        g_theme.dotOff = g_theme.numTextEmpty = g_theme.dim = g_theme.off = faint;
        g_theme.numberLine = g_theme.divider = g_theme.cardLine = 0x1082;
        g_theme.accentLight = g_theme.accentSwitch = g_theme.accentClimate = text;
        g_theme.accentScene = g_theme.accentMedia = g_theme.accentSensor = text;
        g_theme.accentCover = g_theme.iconWell = g_theme.barTrack = faint;
        g_theme.editBg = 0x1082;
        g_theme.editText = selected;
    }
}

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int SB_H  = 24;

static constexpr int MENU_Y = 24;
static constexpr int MENU_ROW_H = 27;
static constexpr int MENU_TEXT_Y = (MENU_ROW_H - UI_FONT_HEIGHT) / 2;
static constexpr int STATUS_TEXT_Y = (SB_H - UI_FONT_HEIGHT) / 2;

// ---------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------
static EntityState g_cards[MAX_ENTITIES];
static EntityState g_emptyCard;
static int g_page = 0;
static int g_focus = 0;
static bool g_detail = false;
static int g_detailGlobal = -1;
static bool g_haStarted = false;
static bool g_linkBlinkOn = true;

static bool g_haOnline = false;
static bool g_statusTitleDirty = false;
static bool g_statusCenterDirty = false;
static bool g_statusLinkDirty = false;
static bool g_statusBatteryDirty = false;
static int g_lastBatteryPct = -1;
static bool g_lastCharging = false;
static bool g_lastDockInferred = false;
static wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;
static bool g_dirty[MAX_ENTITIES] = {false};

static int pageCount() {
    return constrain((int)g_config.pageCount, 1, MAX_PAGES);
}

String menuStatus() {
    return "page=" + String(g_page + 1) + " pages=" + String(pageCount()) +
           " focus=" + String(g_focus + 1);
}

static int globalSlot(int slot) {
    return g_page * GRID_SLOTS + slot;
}

static EntityState& cardAt(int slot) {
    int index = globalSlot(slot);
    if (index >= 0 && index < g_config.entityCount) return g_cards[index];
    return g_emptyCard;
}

static int g_refreshIndex = -1;
static int g_refreshFail = 0;
static uint32_t g_nextRefresh = 0;
static bool g_refreshPending[MAX_ENTITIES] = {};
static bool g_fullRefreshStarted = false;
static uint32_t g_lastFullRefresh = 0;
static uint32_t g_restRefreshReads = 0, g_fullRefreshes = 0;
static wl_status_t g_prevWifiLink = WL_IDLE_STATUS;

String refreshStatus() {
    unsigned pending = 0;
    for (int i = 0; i < g_config.entityCount; ++i) pending += g_refreshPending[i];
    return "rest_refresh_reads=" + String(g_restRefreshReads) +
           " rest_full_refreshes=" + String(g_fullRefreshes) +
           " rest_refresh_pending=" + String(pending) +
           " rest_full_interval_s=" + String(haStateUpdatesReady() ? 1800 : 30);
}

static bool g_cmdMode = false;
static String g_cmdLine;

static constexpr int AC_ROWS = 7;
static int g_acCursor = 0;
static bool g_acEditing = false;
static float g_acDraftTemp = NAN;

static uint32_t g_lastKeyPoll = 0;
static uint32_t g_lastKeyTime = 0;
static char g_lastKey = 0;

static void keypadInit() {
    // The keypad and IP5306 share GPIO21/22 on Core Basic. M5.begin()
    // already owns this bus; Wire.begin() would install a second driver.
    bool present = M5.In_I2C.scanID((uint8_t)0x08, 100000);
    Serial.printf("[keypad] i2c 0x08 %s\n", present ? "present" : "missing");
}

static uint8_t keypadReadByte() {
    // Both Calculator3 and the older MEGA328 panel return key data directly.
    // A register read would consume Calculator3's pending key interrupt.
    uint8_t value = 0;
    if (!M5.In_I2C.start(0x08, true, 100000)) return 0;
    bool readOk = M5.In_I2C.read(&value, 1, true);
    bool stopOk = M5.In_I2C.stop();
    return readOk && stopOk ? value : 0;
}

static char keypadPoll() {
    uint32_t now = millis();
    uint32_t interval = connectedPowerIdle() ? 100 : 20;
    if (now - g_lastKeyPoll < interval) return 0;
    g_lastKeyPoll = now;

    uint8_t raw = keypadReadByte();
    if (raw == 0 || raw == 0xFF) return 0;

    char c = remapKeypadKey(raw);
    if (c == 0) return 0;

    if (c == g_lastKey && now - g_lastKeyTime < 120) return 0;
    g_lastKey = c;
    g_lastKeyTime = now;
    Serial.printf("[keypad] 0x%02X '%c'\n", raw, c);
    return c;
}

// The LED task owns all runtime strip writes, including while HA HTTP blocks.
static constexpr uint32_t COMMAND_LED_DURATION_MS = 300;
static TaskHandle_t g_commandLedTask = nullptr;
static std::atomic<uint8_t> g_commandLedBrightness{0};
static std::atomic<bool> g_commandLedActive{false};
static std::atomic<uint32_t> g_commandLedCount{0};
static std::atomic<uint32_t> g_commandLedFrames{0};
static std::atomic<uint16_t> g_commandLedPixels{0};

static void commandLedTask(void*) {
    constexpr uint32_t STEP_MS = COMMAND_LED_DURATION_MS / 6;
    uint32_t color = 0;
    for (;;) {
        if (xTaskNotifyWait(0, UINT32_MAX, &color, portMAX_DELAY) != pdTRUE) continue;
        uint32_t startedAt = millis();
        uint32_t frames = 0;
        uint16_t pixels = 0;
        g_commandLedActive.store(true);
        for (;;) {
            uint32_t elapsed = millis() - startedAt;
            if (elapsed >= COMMAND_LED_DURATION_MS) break;
            uint32_t brightness = g_commandLedBrightness.load();
            M5.Led.setAllColor(0, 0, 0);
            for (int row = 0; row < 5; ++row) {
                int distance = (int)(elapsed * 255 / STEP_MS) - row * 255;
                int intensity = distance < 0 ? 255 + distance : 255 - distance / 2;
                intensity = constrain(intensity, 0, 255);
                auto scale = [&](uint8_t channel) -> uint8_t {
                    return (channel * brightness * intensity + 32512) / 65025;
                };
                uint8_t r = scale((color >> 16) & 255);
                uint8_t g = scale((color >> 8) & 255);
                uint8_t b = scale(color & 255);
                // Indices 0 and 9 are at the top; 4 and 5 are at the bottom.
                int left = 4 - row;
                int right = 5 + row;
                M5.Led.setColor(left, r, g, b);
                M5.Led.setColor(right, r, g, b);
                if (r || g || b) pixels |= (1u << left) | (1u << right);
            }
            M5.Led.display();
            ++frames;
            // A new command immediately restarts the sweep from the bottom.
            // Notification output can change even on timeout. Only accept a
            // new color when an actual command notification was received.
            uint32_t nextColor = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &nextColor, pdMS_TO_TICKS(20)) == pdTRUE) {
                color = nextColor;
                startedAt = millis();
                frames = 0;
                pixels = 0;
            }
        }
        M5.Led.setAllColor(0, 0, 0);
        M5.Led.display();
        g_commandLedFrames.store(frames);
        g_commandLedPixels.store(pixels);
        g_commandLedActive.store(false);
    }
}

static void initCommandLeds() {
    auto board = M5.getBoard();
    if (board != m5::board_t::board_M5Stack &&
        board != m5::board_t::board_M5StackCore2) return;
    auto* strip = static_cast<m5::LED_Strip_Class*>(M5.Led.getLedInstancePtr());
    if (!strip) return;
    auto config = strip->getConfig();
    config.led_count = 10;
    strip->setConfig(config);
#if M5UNIFIED_RMT_VERSION == 1
    strip->setBus(std::make_shared<SideLedBus>((int)M5.getPin(m5::pin_name_t::rgb_led)));
#endif
    M5.Led.setAutoDisplay(false);
    if (M5.Led.begin()) {
        // Scale RGB directly: M5.Led's brightness curve would round a 5%
        // screen setting almost to black if the same value were used there.
        M5.Led.setBrightness(255);
        M5.Led.setAllColor(0, 0, 0);
        M5.Led.display();
        if (xTaskCreate(commandLedTask, "ha-leds", 3072, nullptr, 1,
                        &g_commandLedTask) != pdPASS) {
            g_commandLedTask = nullptr;
            Serial.println("[led] command animation task unavailable");
        }
    }
}

String commandLedsStatus() {
    String text = "command_leds_ready=" + String(g_commandLedTask ? 1 : 0);
    text += " command_leds_active=" + String((int)g_commandLedActive.load());
    text += " command_leds_brightness=" + String((unsigned)g_commandLedBrightness.load());
    text += " command_leds_duration_ms=" + String(COMMAND_LED_DURATION_MS);
    text += " command_leds_color565=" + String(g_theme.commandLedColor, HEX);
    text += " command_leds_count=" + String((unsigned long)g_commandLedCount.load());
    text += " command_leds_frames=" + String((unsigned long)g_commandLedFrames.load());
    text += " command_leds_pixels=" + String((unsigned)g_commandLedPixels.load());
    return text;
}

static uint32_t g_lastSoundAt = 0;
static bool g_audioQuiet = false;

static void quietAudio() {
    M5.Speaker.end();
    if (M5.getBoard() == m5::board_t::board_M5Stack) {
        dacDisable(25);
        pinMode(25, OUTPUT);
        digitalWrite(25, LOW);
    }
    g_audioQuiet = true;
}

static void playUiSound(const uint8_t* data, size_t length) {
    setConnectedPowerIdle(false);
    if (M5.Speaker.playWav(data, length, 1, -1, true)) {
        g_lastSoundAt = millis();
        g_audioQuiet = false;
    }
}

static void sfxArrow() { playUiSound(sfx_arrow, sfx_arrow_len); }
static void sfxSelect() { playUiSound(sfx_select, sfx_select_len); }

static uint32_t g_lastActivity = 0;
static BacklightPhase g_backlightPhase = BacklightPhase::Normal;
static int g_backlightOverride = -1;
static int g_appliedBrightness = -1;
static uint8_t g_backlightPercent = 0;

static void connectedPowerTick() {
    if (!g_audioQuiet && !M5.Speaker.isPlaying() && millis() - g_lastSoundAt >= 1000) quietAudio();
    bool idle = (millis() - g_lastActivity >= 15000 || g_backlightPhase != BacklightPhase::Normal)
                && !M5.Speaker.isPlaying();
    setConnectedPowerIdle(idle);
    setPowerPollingIdle(idle);
}

// ---------------------------------------------------------------------
//  Text helpers
// ---------------------------------------------------------------------
static void selectUiFont(LovyanGFX& g, uint8_t size) {
    g.setFont(uiFont());
    g.setTextSize(size / 2.0f);
}

static void txt(LovyanGFX& g, const char* s, int x, int y,
                uint16_t color, uint8_t size = 2) {
    g.setTextDatum(TL_DATUM);
    selectUiFont(g, size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

static void txtC(LovyanGFX& g, const char* s, int x, int y,
                 uint16_t color, uint8_t size = 2) {
    g.setTextDatum(MC_DATUM);
    selectUiFont(g, size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

static void txtR(LovyanGFX& g, const char* s, int x, int y,
                 uint16_t color, uint8_t size = 2) {
    g.setTextDatum(TR_DATUM);
    selectUiFont(g, size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

// Truncate by pixel width so text never overlaps the next field.
static String fitText(LovyanGFX& g, const String& in, int maxWidth,
                      uint8_t size) {
    if (in.isEmpty() || maxWidth <= 0) return String("");
    selectUiFont(g, size);
    String out = ui_text::forFont(in);
    if (g.textWidth(out.c_str()) <= maxWidth) return out;
    if (g.textWidth("~") > maxWidth) return String("");
    while (!out.isEmpty()) {
        out.remove(ui_text::previousBoundary(out.c_str(), out.length()));
        if (g.textWidth((out + "~").c_str()) <= maxWidth) break;
    }
    return out + "~";
}

static String shorten(const String& in, size_t maxChars) {
    return ui_text::shorten(in, maxChars);
}

static String upperText(const String& in) {
    String s = in;
    for (size_t i=0; i<s.length(); ++i)
        if (s[i]>='a' && s[i]<='z') s[i]=s[i]-'a'+'A';
    return s;
}

static String tempText(float value) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1fC", value);
    return String(buf);
}

static String displayUnit(const String& unit) {
    if (unit == "°C" || unit == "℃") return String("C");
    if (unit == "°F" || unit == "℉") return String("F");
    return unit;
}

static bool validTemp(float value) {
    return !isnan(value) && value > -100.0f && value < 200.0f;
}

// ---------------------------------------------------------------------
//  Backlight schedule
// ---------------------------------------------------------------------
static void applyBacklightPercent(int percent) {
    percent = constrain(percent, 0, 100);
    g_backlightPercent = (uint8_t)percent;
    int pwm = (percent * 255 + 50) / 100;
    if (pwm != g_appliedBrightness) {
        M5.Display.setBrightness((uint8_t)pwm);
        g_appliedBrightness = pwm;
    }
    g_commandLedBrightness.store((uint8_t)pwm);
}

static void renderInitial();
static void scheduleTick(bool explicitInput = false);

static void noteActivity() {
    setConnectedPowerIdle(false);
    setPowerPollingIdle(false);
    g_lastActivity = millis();
    g_backlightOverride = -1;
    scheduleTick(true);
}

static bool inTimeRange(int now, int start, int end) {
    if (start == end) return false;
    if (start < end) return now >= start && now < end;
    return now >= start || now < end;
}

static void applyBacklightPlan(BacklightPlan plan) {
    bool waking = g_backlightPhase == BacklightPhase::Off && plan.phase != BacklightPhase::Off;
    // Preserve the current palette while off; repaint it before lighting the
    // screen on wake. Entering dim mode reduces the backlight before repaint.
    bool dimTheme = plan.phase == BacklightPhase::Off ? g_dimTheme
                                                     : plan.phase == BacklightPhase::Dim;
    if (waking || dimTheme != g_dimTheme) {
        if (dimTheme || waking) applyBacklightPercent(waking ? 0 : plan.brightness);
        g_dimTheme = dimTheme;
        applyTheme();
        renderInitial();
    }
    g_backlightPhase = plan.phase;
    applyBacklightPercent(plan.brightness);
}

static void scheduleTick(bool explicitInput) {
    struct tm timeinfo;
    bool clockReady = haGetLocalTime(timeinfo);
    int period = -1;
    if (clockReady) {
        int now = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        bool isDay = inTimeRange(now, g_config.dayStart, g_config.dayEnd);
        bool isNight = inTimeRange(now, g_config.nightStart, g_config.nightEnd);
        period = isDay ? 0 : (isNight ? 1 : 0);
    }
    bool night = period == 1;
    uint8_t brightness = clockReady ? (night ? g_config.nightBrightness : g_config.dayBrightness)
                                    : g_config.brightness;
    uint8_t dimBrightness = night ? g_config.nightDimBrightness : g_config.dayDimBrightness;
    BacklightPlan plan = {BacklightPhase::Normal, brightness};
    if (clockReady) {
        plan = planBacklight(millis() - g_lastActivity, brightness, dimBrightness,
                             night ? g_config.nightDimSeconds : g_config.dayDimSeconds,
                             night ? g_config.nightOffSeconds : g_config.dayOffSeconds);
    }
    if (g_backlightOverride == (int)BacklightPhase::Dim) {
        plan = {BacklightPhase::Dim, dimBrightness < brightness ? dimBrightness : brightness};
    } else if (g_backlightOverride == (int)BacklightPhase::Off) {
        plan = {BacklightPhase::Off, 0};
    }
    if (!explicitInput) {
        plan = limitAutomaticBacklight(plan, {g_backlightPhase, g_backlightPercent});
    }
    applyBacklightPlan(plan);
}

bool setDisplayMode(const String& mode) {
    if (mode == "wake") {
        noteActivity();
    } else if (mode == "dim" || mode == "off") {
        scheduleTick();
        g_backlightOverride = (int)(mode == "dim" ? BacklightPhase::Dim : BacklightPhase::Off);
        scheduleTick(true);
    } else {
        return false;
    }
    return true;
}

String backlightStatus() {
    const char* state = g_backlightPhase == BacklightPhase::Dim ? "dim"
                         : (g_backlightPhase == BacklightPhase::Off ? "off" : "normal");
    return "backlight=" + String(state) + " backlight_pwm=" + String(g_appliedBrightness) +
           " dim_theme=" + String((int)g_dimTheme) +
           " display_override=" + String(g_backlightOverride) +
           " lcd_sleep=0" +
           " audio_quiet=" + String(g_audioQuiet ? 1 : 0) +
           " power_sample_interval_ms=" + String(powerSampleIntervalMs()) +
           " keypad_poll_ms=" + String(connectedPowerIdle() ? 100 : 20);
}

// ---------------------------------------------------------------------
//  Icons
// ---------------------------------------------------------------------
static void animateHaCommandLeds() {
    if (!g_commandLedTask) return;
    uint16_t color = g_theme.commandLedColor;
    uint32_t r = ((color >> 11) & 31) * 255 / 31;
    uint32_t g = ((color >> 5) & 63) * 255 / 63;
    uint32_t b = (color & 31) * 255 / 31;
    g_commandLedCount.fetch_add(1);
    xTaskNotify(g_commandLedTask, (r << 16) | (g << 8) | b, eSetValueWithOverwrite);
}

static uint16_t typeAccent(EntityType type) {
    switch (type) {
        case EntityType::Light:   return g_theme.accentLight;
        case EntityType::Switch:  return g_theme.accentSwitch;
        case EntityType::Climate: return g_theme.accentClimate;
        case EntityType::Scene:   return g_theme.accentScene;
        case EntityType::Media:   return g_theme.accentMedia;
        case EntityType::Sensor:  return g_theme.accentSensor;
        case EntityType::Cover:   return g_theme.accentCover;
        default:                  return g_theme.dim;
    }
}

static void iconBulb(LovyanGFX& g, int x, int y, bool on, uint16_t accent) {
    uint16_t c = on ? accent : g_theme.off;
    int cx = x + 14, cy = y + 11;
    g.fillCircle(cx, cy, 8, c);
    g.fillRect(cx - 4, cy + 6, 8, 4, c);
    g.drawFastHLine(cx - 3, cy + 11, 6, c);
    g.drawFastHLine(cx - 2, cy + 13, 4, c);
    if (on) {
        g.drawLine(cx - 12, cy - 7, cx - 15, cy - 10, c);
        g.drawLine(cx + 12, cy - 7, cx + 15, cy - 10, c);
        g.drawLine(cx, cy - 12, cx, cy - 16, c);
    }
}

static void iconClimate(LovyanGFX& g, int x, int y, bool on, uint16_t accent) {
    uint16_t c = on ? accent : g_theme.off;
    int cx = x + 14, cy = y + 14;
    g.drawFastVLine(cx, cy - 11, 22, c);
    g.drawLine(cx - 9, cy - 6, cx + 9, cy + 6, c);
    g.drawLine(cx - 9, cy + 6, cx + 9, cy - 6, c);
    g.drawFastHLine(cx - 4, cy - 11, 8, c);
    g.drawFastHLine(cx - 4, cy + 10, 8, c);
    g.fillCircle(cx, cy, 2, c);
}

static void iconMedia(LovyanGFX& g, int x, int y, bool on, uint16_t accent) {
    uint16_t c = on ? accent : g_theme.off;
    g.drawRect(x + 2, y + 5, 24, 16, c);
    g.drawRect(x + 3, y + 6, 22, 14, c);
    g.fillTriangle(x + 12, y + 9, x + 12, y + 17, x + 19, y + 13, c);
    g.fillRect(x + 8, y + 23, 12, 2, c);
    g.fillRect(x + 5, y + 26, 18, 2, c);
}

static void iconScene(LovyanGFX& g, int x, int y, uint16_t accent) {
    int cx = x + 14, cy = y + 13;
    g.fillTriangle(cx, cy - 12, cx - 4, cy, cx + 4, cy, accent);
    g.fillTriangle(cx, cy + 11, cx - 4, cy, cx + 4, cy, accent);
    g.fillTriangle(cx - 12, cy, cx, cy - 4, cx, cy + 4, accent);
    g.fillTriangle(cx + 11, cy, cx, cy - 4, cx, cy + 4, accent);
    g.fillCircle(cx, cy, 2, accent);
    g.fillCircle(cx + 11, cy - 9, 1, accent);
}

static void iconSensor(LovyanGFX& g, int x, int y, uint16_t accent) {
    int cx = x + 14, cy = y + 15;
    g.drawArc(cx, cy, 9, 11, 150, 390, accent);
    g.drawLine(cx, cy, cx + 6, cy - 5, accent);
    g.fillCircle(cx, cy, 2, accent);
}

static void iconPower(LovyanGFX& g, int x, int y, uint16_t color) {
    int cx = x + 14, cy = y + 14;
    g.drawArc(cx, cy, 9, 11, 120, 420, color);
    g.fillRect(cx - 1, cy - 11, 3, 10, color);
}

static void iconCover(LovyanGFX& g, int x, int y, uint16_t accent) {
    int cx = x + 13, cy = y + 14;
    g.drawRect(cx - 8, cy - 10, 17, 21, accent);
    g.drawFastHLine(cx - 7, cy - 6, 15, accent);
    g.drawFastHLine(cx - 7, cy - 2, 15, accent);
    g.drawFastHLine(cx - 7, cy + 2, 15, accent);
    g.drawFastHLine(cx - 7, cy + 6, 15, accent);
    g.fillCircle(cx + 5, cy + 8, 1, accent);
}

static void iconFor(LovyanGFX& g, int x, int y, const EntityState& e) {
    uint16_t accent = e.available ? typeAccent(e.type) : g_theme.off;
    switch (e.type) {
        case EntityType::Light:   iconBulb(g, x, y, e.on, accent); break;
        case EntityType::Switch:  iconPower(g, x, y, e.on ? accent : g_theme.off); break;
        case EntityType::Climate: iconClimate(g, x, y, e.on, accent); break;
        case EntityType::Media:   iconMedia(g, x, y, e.on, accent); break;
        case EntityType::Scene:   iconScene(g, x, y, accent); break;
        case EntityType::Sensor:  iconSensor(g, x, y, accent); break;
        case EntityType::Cover:   iconCover(g, x, y, accent); break;
        default:                  iconSensor(g, x, y, g_theme.off); break;
    }
}

// ---------------------------------------------------------------------
//  Entity text
// ---------------------------------------------------------------------
static String displayName(const EntityState& e) {
    if (e.id.isEmpty()) return String("EMPTY");
    if (!e.name.isEmpty()) return e.name;
    return defaultNameFromId(e.id);
}

static int lightPercent(const EntityState& e) {
    if (e.brightness < 0) return -1;
    return constrain((e.brightness * 100 + 127) / 255, 0, 100);
}

static String entityStateText(const EntityState& e) {
    if (e.id.isEmpty()) return String("");
    if (!e.available) return String("--");

    switch (e.type) {
        case EntityType::Light: {
            if (!e.on) return String("OFF");
            int pct = lightPercent(e);
            if (pct >= 0) return String("ON ") + String(pct) + "%";
            return String("ON");
        }
        case EntityType::Switch:
            return e.on ? String("ON") : String("OFF");
        case EntityType::Climate: {
            String s = upperText(e.state);
            if (s == "HEAT_COOL") s = "HEAT/COOL";
            else if (s == "FAN_ONLY") s = "FAN";
            // The menu adjusts the setpoint with +/-; display that same value.
            // The measured room temperature remains in AC detail's CURRENT row.
            float t = validTemp(e.target) ? e.target
                                         : (validTemp(e.current) ? e.current : NAN);
            if (validTemp(t)) s += " " + tempText(t);
            return s;
        }
        case EntityType::Media:
            return upperText(e.state);
        case EntityType::Scene:
            return String("SCENE");
        case EntityType::Sensor: {
            if (e.state == "on" && (e.detail == "door" || e.detail == "window")) {
                return String("OPEN");
            }
            if (e.state == "off" && (e.detail == "door" || e.detail == "window")) {
                return String("CLOSED");
            }
            String s = e.state;
            if (!e.unit.isEmpty()) s += " " + displayUnit(e.unit);
            return s;
        }
        case EntityType::Cover:
            return upperText(e.state);
        default:
            return upperText(e.state);
    }
}

static uint16_t entityStateColor(const EntityState& e) {
    if (e.id.isEmpty() || !e.available) return g_theme.off;
    switch (e.type) {
        case EntityType::Light:
        case EntityType::Switch:
        case EntityType::Climate:
        case EntityType::Media:
        case EntityType::Cover:
            return e.on ? typeAccent(e.type) : g_theme.off;
        default:
            return typeAccent(e.type);
    }
}

// ---------------------------------------------------------------------
//  Status bar
// ---------------------------------------------------------------------
static void drawBattery(LovyanGFX& g, int x, int y, int pct, bool charging,
                        bool dockInferred) {
    // Monochrome battery icon, same color as the signal bars.
    const uint16_t mono = g_theme.barText;
    g.drawRect(x, y, 18, 12, mono);
    g.drawRect(x - 1, y - 1, 20, 14, mono);
    g.fillRect(x + 20, y + 4, 2, 5, mono);

    if (dockInferred) {
        // Plug overlay means assumed dock power, with no charging/full claim.
        g.fillRect(x + 6, y + 2, 2, 3, mono);
        g.fillRect(x + 10, y + 2, 2, 3, mono);
        g.fillRect(x + 5, y + 5, 8, 4, mono);
        g.fillRect(x + 8, y + 9, 2, 2, mono);
        return;
    }

    if (charging) {
        g.fillTriangle(x + 10, y + 2, x + 6, y + 6, x + 9, y + 6, mono);
        g.fillTriangle(x + 9, y + 6, x + 6, y + 10, x + 11, y + 6, mono);
        return;
    }

    if (pct < 0) {
        // Unavailable telemetry must not look like a depleted battery.
        g.drawPixel(x + 8, y + 3, mono);
        g.drawPixel(x + 9, y + 3, mono);
        g.drawPixel(x + 10, y + 4, mono);
        g.drawPixel(x + 9, y + 5, mono);
        g.drawPixel(x + 8, y + 6, mono);
        g.drawPixel(x + 8, y + 8, mono);
        return;
    }

    int fill = (14 * constrain(pct, 0, 100)) / 100;
    if (fill > 0) g.fillRect(x + 2, y + 2, fill, 8, mono);
}

// Combined Wi-Fi + HA link indicator; blink while either connection is down.
static void drawLinkIcon(LovyanGFX& g, int x, int y) {
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    bool linkUp = wifiUp && g_haOnline;

    // Connecting, retrying or disconnected: blink the signal icon.
    if (!linkUp && !g_linkBlinkOn) return;

    int rssi = wifiUp ? WiFi.RSSI() : -100;
    int bars = 0;
    if (wifiUp) {
        if (rssi > -55) bars = 4;
        else if (rssi > -65) bars = 3;
        else if (rssi > -75) bars = 2;
        else bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        uint16_t c = (i < bars) ? g_theme.barText : g_theme.dotOff;
        g.fillRect(x + i * 4, y + 13 - h, 3, h, c);
    }
}

static void drawPageDots(LovyanGFX& g) {
    int pages = pageCount();
    int totalW = pages * 6 + (pages - 1) * 4;
    int startX = 160 - totalW / 2;
    for (int p = 0; p < pages; p++) {
        g.fillRect(startX + p * 10, 9, 6, 6,
                   (p == g_page) ? g_theme.dotOn : g_theme.dotOff);
    }
}

static void drawStatusTitle(LovyanGFX& g) {
    String title;
    bool acDetail = g_detail && g_detailGlobal >= 0 &&
                    g_detailGlobal < g_config.entityCount &&
                    g_cards[g_detailGlobal].type == EntityType::Climate;
    if (acDetail) {
        title = upperText(displayName(g_cards[g_detailGlobal]));
    } else {
        title = (g_page < MAX_PAGES) ? g_config.pageTitles[g_page] : String("");
        if (title.isEmpty()) title = "PAGE" + String(g_page + 1);
    }
    title = fitText(g, title, 86, 2);
    txt(g, title.c_str(), 6, STATUS_TEXT_Y, g_theme.barText, 2);
}

static void drawStatusCenter(LovyanGFX& g) {
    if (g_detail) return;
    drawPageDots(g);
}

static void drawStatusBar(LovyanGFX& g) {
    g.fillRect(0, 0, SCR_W, SB_H, g_theme.barBg);
    drawStatusTitle(g);
    drawStatusCenter(g);
    drawLinkIcon(g, 270, 5);
    PowerStatus power = readPowerStatus();
    drawBattery(g, 294, 6, power.batteryPercent,
                power.charging == ChargeState::Charging, power.dockInferred);
}

// ---------------------------------------------------------------------
//  Menu list
// ---------------------------------------------------------------------
static void drawThemeDivider(LovyanGFX& g, int y) {
    if (g_theme.dotted) {
        for (int x = 0; x < SCR_W; x += 3) {
            g.drawPixel(x, y, g_theme.divider);
        }
    } else {
        g.drawFastHLine(0, y, SCR_W, g_theme.divider);
    }
}

// Cache only the physical display. Screenshot rendering must not change what
// we believe is currently on screen.
struct RowPaint {
    String label, value;
    int labelWidth = 0, valueLeft = 314;
    uint16_t bg = 0, labelColor = 0, valueColor = 0;
    uint16_t numberBg = 0, numberColor = 0;
    bool valid = false;
};
static RowPaint g_rowPaint[GRID_SLOTS];

static void paintListRow(LovyanGFX& g, int slot, const RowPaint& row,
                         bool forceFull) {
    RowPaint& previous = g_rowPaint[slot];
    bool display = (&g == &M5.Display);
    bool full = forceFull || !display || !previous.valid ||
                previous.bg != row.bg || previous.numberBg != row.numberBg ||
                previous.numberColor != row.numberColor;
    int y = MENU_Y + slot * MENU_ROW_H;
    bool labelChanged = full || previous.label != row.label ||
                        previous.labelColor != row.labelColor;
    bool valueChanged = full || previous.value != row.value ||
                        previous.valueColor != row.valueColor;
    if (full) {
        g.fillRect(0, y, SCR_W, MENU_ROW_H, row.bg);
        g.fillRect(0, y, 24, MENU_ROW_H, row.numberBg);
        if (g_theme.numberLine != 0) {
            g.drawFastVLine(24, y, MENU_ROW_H, g_theme.numberLine);
        }
        char number[2] = { char('1' + slot), 0 };
        const auto ink = uiFontInkBounds(number[0]);
        // Center visible pixels; leave the bottom divider outside the content.
        const int numberX = (24 - ink.width) / 2 - ink.x;
        const int numberY = (MENU_ROW_H - 1 - ink.height) / 2 - ink.y;
        txt(g, number, numberX, y + numberY, row.numberColor, 2);
        if (slot < GRID_SLOTS - 1) drawThemeDivider(g, y + MENU_ROW_H - 1);
    } else {
        selectUiFont(g, 2);
        // Clear both old and new bounds before drawing either field. A value
        // growing wider can also shorten a long label beside it.
        if (labelChanged) {
            int width = max(previous.labelWidth, row.labelWidth);
            if (width > 0) g.fillRect(32, y + MENU_TEXT_Y, width, g.fontHeight(), row.bg);
        }
        if (valueChanged) {
            int left = min(previous.valueLeft, row.valueLeft);
            if (left < 314) g.fillRect(left, y + MENU_TEXT_Y, 315 - left, g.fontHeight(), row.bg);
        }
    }
    if (labelChanged && !row.label.isEmpty()) {
        txt(g, row.label.c_str(), 32, y + MENU_TEXT_Y, row.labelColor, 2);
    }
    if (valueChanged && !row.value.isEmpty()) {
        txtR(g, row.value.c_str(), 314, y + MENU_TEXT_Y, row.valueColor, 2);
    }
    if (display) {
        previous = row;
        previous.valid = true;
    }
}

static void drawMenuRow(LovyanGFX& g, int slot, const EntityState& e,
                        bool selected, bool forceFull = true) {
    bool empty = e.id.isEmpty();
    RowPaint row;
    row.bg = selected ? g_theme.rowSel : g_theme.rowBg;
    row.numberBg = selected ? g_theme.numSelBg
                            : (empty ? g_theme.numBgEmpty : g_theme.numBg);
    row.numberColor = selected ? g_theme.numSelText
                               : (empty ? g_theme.numTextEmpty : g_theme.numText);
    row.labelColor = selected ? g_theme.rowSelText : g_theme.rowText;
    row.valueColor = selected ? g_theme.rowSelText : entityStateColor(e);
    if (!empty) {
        row.value = fitText(g, entityStateText(e), 140, 2);
        row.valueLeft = 314 - (int)g.textWidth(row.value.c_str());
        int nameWidth = max(90, row.valueLeft - 32 - 8);
        row.label = fitText(g, displayName(e), nameWidth, 2);
        row.labelWidth = g.textWidth(row.label.c_str());
    }
    paintListRow(g, slot, row, forceFull);
}

static void drawMenu(LovyanGFX& g) {
    for (int slot = 0; slot < GRID_SLOTS; slot++) {
        drawMenuRow(g, slot, cardAt(slot), slot == g_focus);
    }
}

// ---------------------------------------------------------------------
//  AC detail view
// ---------------------------------------------------------------------
static bool acDetailActive() {
    return g_detail && g_detailGlobal >= 0 &&
           g_detailGlobal < g_config.entityCount &&
           g_cards[g_detailGlobal].type == EntityType::Climate;
}

static EntityState& acEntity() {
    return g_cards[g_detailGlobal];
}

static String modeDisplay(const String& value) {
    String mode = upperText(value);
    if (mode == "FAN_ONLY") return String("FAN");
    if (mode == "HEAT_COOL") return String("HEAT/COOL");
    return mode;
}

static bool csvHas(const String& csv, const String& value) {
    if (csv.isEmpty()) return true;
    int start = 0;
    while (start <= (int)csv.length()) {
        int comma = csv.indexOf(',', start);
        String item = comma < 0 ? csv.substring(start) : csv.substring(start, comma);
        item.trim();
        if (item.equalsIgnoreCase(value)) return true;
        if (comma < 0) break;
        start = comma + 1;
    }
    return false;
}

static String nextCsv(const String& csv, const String& current) {
    String values[12];
    int count = 0;
    int start = 0;
    while (start <= (int)csv.length() && count < 12) {
        int comma = csv.indexOf(',', start);
        String item = comma < 0 ? csv.substring(start) : csv.substring(start, comma);
        item.trim();
        if (!item.isEmpty()) values[count++] = item;
        if (comma < 0) break;
        start = comma + 1;
    }
    if (count == 0) return String("");
    for (int i = 0; i < count; i++) {
        if (values[i].equalsIgnoreCase(current)) return values[(i + 1) % count];
    }
    return values[0];
}

static String acModeCycle(const EntityState& e, int index) {
    uint8_t mask = (index >= 0 && index < MAX_ENTITIES)
                       ? g_config.climateModes[index]
                       : 0;
    String list = "off";
    auto add = [&](uint8_t bit, const char* mode) {
        if ((mask & bit) && csvHas(e.hvacModes, mode)) {
            list += ",";
            list += mode;
        }
    };
    add(CLIMATE_COOL, "cool");
    add(CLIMATE_HEAT, "heat");
    add(CLIMATE_DRY, "dry");
    add(CLIMATE_FAN, "fan_only");
    add(CLIMATE_AUTO, "auto");
    return list;
}

static String acModeText(const EntityState& e) {
    if (!e.available) return "--";
    return modeDisplay(e.state);
}

static String acDisplayModeValue(String value) {
    value = upperText(value);
    value.replace("_", " ");
    return value;
}

static String acFanText(const EntityState& e) {
    if (!e.available || e.fanMode.isEmpty()) return "--";
    return acDisplayModeValue(e.fanMode);
}

static String acSwingText(const EntityState& e) {
    if (!e.available || e.swingMode.isEmpty()) return "--";
    return acDisplayModeValue(e.swingMode);
}

static String acActionText(const EntityState& e) {
    if (!e.available) return "--";
    String action = e.hvacAction.isEmpty() ? e.state : e.hvacAction;
    if (action.isEmpty()) return "--";
    return acDisplayModeValue(action);
}

static String acAuxText(const EntityState& e) {
    if (e.aux.id.isEmpty() || !e.aux.available) return "--";
    switch (e.aux.type) {
        case EntityType::Light: {
            if (!e.aux.on) return String("OFF");
            if (e.aux.brightness < 0) return String("ON");
            int pct = constrain((e.aux.brightness * 100 + 127) / 255, 0, 100);
            return "ON " + String(pct) + "%";
        }
        case EntityType::Switch:
            return e.aux.on ? String("ON") : String("OFF");
        case EntityType::Scene:
            return String("SCENE");
        case EntityType::Media:
        case EntityType::Cover:
            return upperText(e.aux.state);
        case EntityType::Sensor: {
            String value = e.aux.state;
            if (!e.aux.unit.isEmpty()) value += " " + displayUnit(e.aux.unit);
            return value;
        }
        default:
            return e.aux.state.isEmpty() ? String("--") : upperText(e.aux.state);
    }
}

static String acAuxLabel(const EntityState& e, int index) {
    if (e.aux.id.isEmpty()) return String("");
    String label = (index >= 0 && index < MAX_ENTITIES)
                       ? g_config.auxNames[index]
                       : String("");
    if (label.isEmpty()) label = e.aux.name;
    if (label.isEmpty()) label = "AUX";
    return shorten(upperText(label), 12);
}

static String acTempText(const EntityState& e, bool editing) {
    if (editing && validTemp(g_acDraftTemp)) return tempText(g_acDraftTemp);
    if (validTemp(e.target)) return tempText(e.target);
    return String("--");
}

static String acCurrentText(const EntityState& e) {
    return validTemp(e.current) ? tempText(e.current) : String("--");
}

static void drawAcRow(LovyanGFX& g, int slot, const String& label,
                      const String& value, bool selected, bool editing,
                      bool forceFull = false) {
    RowPaint row;
    row.bg = editing ? g_theme.editBg
                     : (selected ? g_theme.rowSel : g_theme.rowBg);
    row.numberBg = editing ? g_theme.editBg
                           : (selected ? g_theme.numSelBg : g_theme.numBg);
    row.numberColor = editing ? g_theme.editText
                              : (selected ? g_theme.numSelText : g_theme.numText);
    row.labelColor = editing ? g_theme.editText
                             : (selected ? g_theme.rowSelText : g_theme.rowText);
    row.valueColor = editing ? g_theme.editText
                             : (selected ? g_theme.rowSelText : g_theme.text);
    row.label = fitText(g, label, 128, 2);
    row.labelWidth = g.textWidth(row.label.c_str());
    int valueWidth = max(80, 314 - 32 - 8 - row.labelWidth);
    row.value = fitText(g, value, valueWidth, 2);
    row.valueLeft = 314 - (int)g.textWidth(row.value.c_str());
    paintListRow(g, slot, row, forceFull);
}

static String acLabelFor(const EntityState& e, int index, int slot) {
    switch (slot) {
        case 0: return "MODE";
        case 1: return "TEMP";
        case 2: return "FAN";
        case 3: return "SWING";
        case 4: return "ACTION";
        case 5: return acAuxLabel(e, index);
        case 6: return "CURRENT";
        default: return "";
    }
}

static String acValueFor(const EntityState& e, int slot) {
    switch (slot) {
        case 0: return acModeText(e);
        case 1: return acTempText(e, g_acEditing);
        case 2: return acFanText(e);
        case 3: return acSwingText(e);
        case 4: return acActionText(e);
        case 5: return acAuxText(e);
        case 6: return acCurrentText(e);
        default: return "";
    }
}

static void drawAcDetail(LovyanGFX& g, const EntityState& e, bool forceFull = true) {
    // Keep all eight numbered rows visible; only the first seven have controls.
    for (int i = 0; i < GRID_SLOTS; i++) {
        drawAcRow(g, i, acLabelFor(e, g_detailGlobal, i),
                  acValueFor(e, i), i == g_acCursor,
                  i == 1 && g_acEditing, forceFull);
    }
}

// ---------------------------------------------------------------------
//  Render
// ---------------------------------------------------------------------
static void renderAll(LovyanGFX& g) {
    selectUiFont(g, 2);
    // Clear to black before repainting the final theme colors.
    g.fillScreen(TFT_BLACK);
    drawStatusBar(g);

    if (acDetailActive()) {
        drawAcDetail(g, acEntity());
    } else {
        drawMenu(g);
    }
}

// Full draw for boot and palette transitions. Value updates use partial redraws.
static void renderInitial() {
    M5.Display.startWrite();
    renderAll(M5.Display);
    M5.Display.endWrite();
}

static void redrawContent() {
    M5.Display.startWrite();
    M5.Display.fillRect(0, SB_H, SCR_W, SCR_H - SB_H, TFT_BLACK);
    if (acDetailActive()) {
        drawAcDetail(M5.Display, acEntity());
    } else {
        drawMenu(M5.Display);
    }
    M5.Display.endWrite();
}

static void redrawMenuPage() {
    // All eight rows fill their cells completely, so no full-screen clear is
    // needed when flipping pages. This removes the black flash.
    M5.Display.startWrite();
    drawMenu(M5.Display);
    M5.Display.endWrite();
}

static void redrawAcDetail() {
    if (!acDetailActive()) return;
    M5.Display.startWrite();
    drawAcDetail(M5.Display, acEntity(), false);
    M5.Display.endWrite();
}

static void redrawAcRow(int slot) {
    if (!acDetailActive() || slot < 0 || slot >= AC_ROWS) return;
    EntityState& e = acEntity();
    M5.Display.startWrite();
    drawAcRow(M5.Display, slot, acLabelFor(e, g_detailGlobal, slot),
              acValueFor(e, slot), slot == g_acCursor,
              slot == 1 && g_acEditing);
    M5.Display.endWrite();
}

static void redrawRow(int slot, bool selected) {
    M5.Display.startWrite();
    drawMenuRow(M5.Display, slot, cardAt(slot), selected, false);
    M5.Display.endWrite();
}

static void redrawStatusRegions() {
    if (!g_statusTitleDirty && !g_statusCenterDirty &&
        !g_statusLinkDirty && !g_statusBatteryDirty) {
        return;
    }

    M5.Display.startWrite();
    if (g_statusTitleDirty) {
        M5.Display.fillRect(0, 0, 96, SB_H, g_theme.barBg);
        drawStatusTitle(M5.Display);
    }
    if (g_statusCenterDirty) {
        M5.Display.fillRect(92, 0, 144, SB_H, g_theme.barBg);
        drawStatusCenter(M5.Display);
    }
    if (g_statusLinkDirty) {
        M5.Display.fillRect(264, 0, 26, SB_H, g_theme.barBg);
        drawLinkIcon(M5.Display, 270, 5);
    }
    if (g_statusBatteryDirty) {
        M5.Display.fillRect(290, 0, 30, SB_H, g_theme.barBg);
        PowerStatus power = readPowerStatus();
        drawBattery(M5.Display, 294, 6, power.batteryPercent,
                    power.charging == ChargeState::Charging, power.dockInferred);
    }
    M5.Display.endWrite();

    g_statusTitleDirty = false;
    g_statusCenterDirty = false;
    g_statusLinkDirty = false;
    g_statusBatteryDirty = false;
}

static void renderDirty() {
    if (g_backlightPhase == BacklightPhase::Off) return;  // Wake repaints cached state.
    if (g_detail) {
        bool detailDirty = (g_detailGlobal >= 0 && g_dirty[g_detailGlobal]);
        for (int i = 0; i < MAX_ENTITIES; i++) g_dirty[i] = false;
        if (detailDirty) {
            redrawAcDetail();
        }
        redrawStatusRegions();
        return;
    }

    M5.Display.startWrite();
    for (int slot = 0; slot < GRID_SLOTS; slot++) {
        int index = globalSlot(slot);
        if (index >= g_config.entityCount) continue;
        if (!g_dirty[index]) continue;
        g_dirty[index] = false;
        drawMenuRow(M5.Display, slot, g_cards[index], slot == g_focus, false);
    }
    M5.Display.endWrite();

    redrawStatusRegions();
}

// ---------------------------------------------------------------------
//  WebSocket callbacks
// ---------------------------------------------------------------------
static void onHaState(const EntityState& state) {
    for (int i = 0; i < g_config.entityCount; i++) {
        EntityState& card = g_cards[i];
        if (card.id == state.id) {
            AuxEntity aux = card.aux;
            String custom = card.customName;
            card = state;
            card.aux = aux;
            card.customName = custom;
            if (!custom.isEmpty()) card.name = custom;
            if (i / GRID_SLOTS == g_page || (g_detail && i == g_detailGlobal)) {
                g_dirty[i] = true;
            }
        }
        if (!card.aux.id.isEmpty() && card.aux.id == state.id) {
            card.aux.revision = state.revision;
            card.aux.name = state.name;
            card.aux.state = state.state;
            card.aux.unit = state.unit;
            card.aux.detail = state.detail;
            card.aux.type = state.type;
            card.aux.on = state.on;
            card.aux.available = state.available;
            card.aux.brightness = state.brightness;
            card.aux.brightnessAdjustable = state.brightnessAdjustable;
            if (i / GRID_SLOTS == g_page || (g_detail && i == g_detailGlobal)) {
                g_dirty[i] = true;
            }
            continue;
        }
    }
}

static void onHaStatus(bool online) {
    if (g_haOnline != online) {
        g_haOnline = online;
        g_statusLinkDirty = true;
    }
}

static void onHaAuth() {
    // Re-sync through REST after a successful WebSocket subscription.
    g_refreshIndex = -1;
    g_nextRefresh = 0;
    g_fullRefreshStarted = false;
}

// ---------------------------------------------------------------------
//  Actions
// ---------------------------------------------------------------------
static void scheduleRefresh() {
    g_refreshIndex = -1;
    g_nextRefresh = millis() + 1500;
}

static void requestEntityRefresh(const String& id) {
    for (int i = 0; i < g_config.entityCount; ++i) {
        if (g_cards[i].id == id || g_cards[i].aux.id == id) g_refreshPending[i] = true;
    }
    if (g_refreshIndex < 0) g_nextRefresh = millis() + 1500;
}

static void goBack() {
    if (g_detail) {
        g_detail = false;
        g_acEditing = false;
        g_statusTitleDirty = true;
        g_statusCenterDirty = true;
        redrawContent();
        redrawStatusRegions();
        sfxArrow();
    }
}

static void openDetail() {
    if (g_detail) return;
    const EntityState& e = cardAt(g_focus);
    if (e.id.isEmpty() || e.type != EntityType::Climate) return;
    g_detail = true;
    g_detailGlobal = globalSlot(g_focus);
    g_acCursor = 0;
    g_acEditing = false;
    g_acDraftTemp = NAN;
    g_statusTitleDirty = true;
    g_statusCenterDirty = true;
    redrawContent();
    redrawStatusRegions();
    sfxSelect();
}

static void moveFocus(int delta) {
    if (g_detail) return;
    if (delta == 0) return;

    int next = g_focus;
    for (int step = 0; step < GRID_SLOTS; step++) {
        next = (next + delta + GRID_SLOTS) % GRID_SLOTS;
        if (next == g_focus) return;
        if (!cardAt(next).id.isEmpty()) {
            int old = g_focus;
            g_focus = next;
            M5.Display.startWrite();
            drawMenuRow(M5.Display, old, cardAt(old), false);
            drawMenuRow(M5.Display, next, cardAt(next), true);
            M5.Display.endWrite();
            sfxArrow();
            return;
        }
    }
}

static EntityState& selectedEntity() {
    if (g_detail && g_detailGlobal >= 0 && g_detailGlobal < g_config.entityCount) {
        return g_cards[g_detailGlobal];
    }
    return cardAt(g_focus);
}

static bool callService(const String& domain, const String& service,
                        const String& entityId, const String& extra = "") {
    bool ok = haCallService(domain, service, entityId, extra);
    if (ok) requestEntityRefresh(entityId);
    return ok;
}

static void acMove(int delta) {
    if (!acDetailActive()) return;
    EntityState& e = acEntity();
    int next = g_acCursor;
    for (int step = 0; step < AC_ROWS; step++) {
        next = (next + delta + AC_ROWS) % AC_ROWS;
        if (next == g_acCursor) return;
        if (next == 5 && e.aux.id.isEmpty()) continue;
        int old = g_acCursor;
        g_acCursor = next;
        g_acEditing = false;
        redrawAcRow(old);
        redrawAcRow(next);
        sfxArrow();
        return;
    }
}

static void acAdjust(int delta) {
    if (!acDetailActive() || !g_acEditing) return;
    EntityState& e = acEntity();
    float base = validTemp(g_acDraftTemp) ? g_acDraftTemp
                                          : (validTemp(e.target) ? e.target : 24.0f);
    g_acDraftTemp = constrain(base + delta * e.tempStep, e.minTemp, e.maxTemp);
    redrawAcRow(1);
    sfxArrow();
}

static bool acNextMode(EntityState& e, int index) {
    String list = acModeCycle(e, index);
    String next = nextCsv(list, e.state);
    if (next.isEmpty() || next.equalsIgnoreCase(e.state)) return false;
    bool ok = callService("climate", "set_hvac_mode", e.id,
                          "\"hvac_mode\":\"" + next + "\"");
    if (ok) {
        e.state = next;
        e.on = (next != "off");
        e.available = true;
    }
    return ok;
}

static void acCycleFan() {
    EntityState& e = acEntity();
    if (e.fanModes.isEmpty()) return;
    String next = nextCsv(e.fanModes, e.fanMode);
    if (next.isEmpty()) return;
    bool ok = callService("climate", "set_fan_mode", e.id,
                          "\"fan_mode\":\"" + next + "\"");
    if (ok) {
        e.fanMode = next;
        redrawAcRow(2);
        scheduleRefresh();
        sfxSelect();
    }
}

static void acCycleSwing() {
    EntityState& e = acEntity();
    if (e.swingModes.isEmpty()) return;
    String next = nextCsv(e.swingModes, e.swingMode);
    if (next.isEmpty()) return;
    bool ok = callService("climate", "set_swing_mode", e.id,
                          "\"swing_mode\":\"" + next + "\"");
    if (ok) {
        e.swingMode = next;
        redrawAcRow(3);
        scheduleRefresh();
        sfxSelect();
    }
}

static void acAuxAction() {
    EntityState& e = acEntity();
    if (e.aux.id.isEmpty()) return;
    bool ok = false;
    String domain = entityDomain(e.aux.id);
    switch (e.aux.type) {
        case EntityType::Light:
            ok = callService(domain, "toggle", e.aux.id);
            if (ok) {
                e.aux.on = !e.aux.on;
                e.aux.state = e.aux.on ? "on" : "off";
            }
            break;
        case EntityType::Switch:
            ok = callService("homeassistant", "toggle", e.aux.id);
            if (ok) {
                e.aux.on = !e.aux.on;
                e.aux.state = e.aux.on ? "on" : "off";
            }
            break;
        case EntityType::Scene:
            ok = callService(domain, "turn_on", e.aux.id);
            break;
        case EntityType::Media:
            ok = callService(domain, "media_play_pause", e.aux.id);
            break;
        case EntityType::Cover:
            ok = callService(domain, "toggle", e.aux.id);
            break;
        default:
            return;
    }
    if (ok) {
        redrawAcRow(5);
        scheduleRefresh();
        sfxSelect();
    }
}

static void acTempAction() {
    EntityState& e = acEntity();
    if (!e.available || !e.temperatureAdjustable) return;
    if (!g_acEditing) {
        g_acEditing = true;
        g_acDraftTemp = validTemp(e.target) ? e.target
                                           : (validTemp(e.current) ? e.current : 24.0f);
        g_acDraftTemp = constrain(g_acDraftTemp, e.minTemp, e.maxTemp);
        redrawAcRow(1);
        sfxSelect();
        return;
    }

    float value = constrain(g_acDraftTemp, e.minTemp, e.maxTemp);
    char extra[40];
    snprintf(extra, sizeof(extra), "\"temperature\":%.2f", value);
    bool ok = callService("climate", "set_temperature", e.id, extra);
    if (ok) {
        e.target = value;
        g_acEditing = false;
        redrawAcDetail();
        scheduleRefresh();
        sfxSelect();
    }
}

static void acAction() {
    if (!acDetailActive()) return;
    if (g_acEditing) {
        acTempAction();
        return;
    }
    switch (g_acCursor) {
        case 0:
            if (acNextMode(acEntity(), g_detailGlobal)) {
                redrawAcDetail();
                scheduleRefresh();
                sfxSelect();
            }
            break;
        case 1: acTempAction(); break;
        case 2: acCycleFan(); break;
        case 3: acCycleSwing(); break;
        case 4: break;
        case 5: acAuxAction(); break;
        case 6: break;
    }
}

static void primaryAction() {
    if (g_detail) {
        acAction();
        return;
    }

    EntityState& e = cardAt(g_focus);
    if (e.id.isEmpty()) return;

    bool ok = false;
    if (e.type != EntityType::Climate && e.type != EntityType::Sensor) {
        sfxSelect();
    }
    String domain = entityDomain(e.id);
    switch (e.type) {
        case EntityType::Light:
            ok = callService(domain, "toggle", e.id);
            if (ok) {
                e.on = !e.on;
                e.state = e.on ? "on" : "off";
            }
            break;
        case EntityType::Switch:
            ok = callService("homeassistant", "toggle", e.id);
            if (ok) {
                e.on = !e.on;
                e.state = e.on ? "on" : "off";
            }
            break;
        case EntityType::Scene:
            ok = callService(domain, "turn_on", e.id);
            break;
        case EntityType::Media:
            ok = callService(domain, "media_play_pause", e.id);
            break;
        case EntityType::Cover:
            ok = callService(domain, "toggle", e.id);
            break;
        case EntityType::Climate:
            ok = acNextMode(e, globalSlot(g_focus));
            if (ok) sfxSelect();
            break;
        case EntityType::Sensor:
        default:
            return;
    }

    if (ok) scheduleRefresh();
    redrawRow(g_focus, true);
    redrawStatusRegions();
}

template<class Light> static bool adjustLightBrightness(Light& e, int delta) {
    if (!delta || e.id.isEmpty() || !e.available || !e.brightnessAdjustable) return false;
    int current = e.on ? (e.brightness < 0 ? 50 : (e.brightness * 100 + 127) / 255) : 0;
    int pct = constrain(current + delta * 10, 0, 100);
    if (pct == current) return false;
    String extra = pct ? "\"brightness_pct\":" + String(pct) : String("");
    if (!callService(entityDomain(e.id), pct ? "turn_on" : "turn_off", e.id, extra)) return false;
    e.on = pct > 0;
    e.state = e.on ? "on" : "off";
    e.brightness = (pct * 255 + 50) / 100;
    return true;
}

static bool adjustClimateTemperature(EntityState& e, int delta) {
    if (!delta || e.id.isEmpty() || !e.available || !e.temperatureAdjustable) return false;
    if (!isfinite(e.minTemp) || !isfinite(e.maxTemp) || e.minTemp > e.maxTemp) return false;
    float step = isfinite(e.tempStep) && e.tempStep > 0 ? e.tempStep : 0.5f;
    float base = validTemp(e.target) ? e.target : (validTemp(e.current) ? e.current : 22.0f);
    float target = constrain(base + delta * step, e.minTemp, e.maxTemp);
    if (validTemp(e.target) && fabsf(target - e.target) < 0.0001f) return false;
    char extra[48];
    snprintf(extra, sizeof(extra), "\"temperature\":%.2f", target);
    if (!callService("climate", "set_temperature", e.id, extra)) return false;
    e.target = target;
    return true;
}

static void adjustSelected(int delta) {
    if (acDetailActive()) {
        if (g_acCursor == 1) {
            if (g_acEditing) {
                acAdjust(delta); // Preserve explicit edit/confirm behavior.
                return;
            }
            if (!adjustClimateTemperature(acEntity(), delta)) return;
            redrawAcRow(1);
        } else if (g_acCursor == 5 && acEntity().aux.type == EntityType::Light) {
            if (!adjustLightBrightness(acEntity().aux, delta)) return;
            redrawAcRow(5);
        } else return;
    } else {
        EntityState& e = selectedEntity();
        bool changed = e.type == EntityType::Light ? adjustLightBrightness(e, delta)
                       : (e.type == EntityType::Climate && adjustClimateTemperature(e, delta));
        if (!changed) return;
        redrawRow(g_focus, true);
    }
    scheduleRefresh();
    redrawStatusRegions();
}

static void changePage(int delta) {
    if (g_detail) return;
    int pages = pageCount();
    if (pages <= 1) return;
    g_page = (g_page + delta + pages) % pages;
    g_focus = 0;
    for (int i = 0; i < MAX_ENTITIES; i++) g_dirty[i] = false;
    g_statusTitleDirty = true;
    g_statusCenterDirty = true;
    redrawMenuPage();
    redrawStatusRegions();
    sfxArrow();
}

static void directSelect(int idx) {
    if (g_detail || idx < 0 || idx >= GRID_SLOTS) return;
    if (globalSlot(idx) >= g_config.entityCount) return;
    if (idx != g_focus) {
        int old = g_focus;
        g_focus = idx;
        M5.Display.startWrite();
        drawMenuRow(M5.Display, old, cardAt(old), false);
        drawMenuRow(M5.Display, idx, cardAt(idx), true);
        M5.Display.endWrite();
    }
    primaryAction();
}

// ---------------------------------------------------------------------
//  Input
// ---------------------------------------------------------------------
static void handleKey(char c) {
    bool wakeOnly = g_backlightPhase == BacklightPhase::Off;
    noteActivity();
    if (wakeOnly) return;

    // Keypad r1c1 (AC): short press sends 'A', long press sends backspace.
    // Always leave the panel, discarding any unconfirmed temperature edit.
    if (c == 'A' || c == '\b') {
        goBack();
        return;
    }
    // Keypad r1c2 / r1c3: previous / next page.
    if (c == 'M' || c == '%') {
        changePage(c == 'M' ? -1 : +1);
        return;
    }
    if (c == '+' || c == '-') {
        adjustSelected(c == '+' ? +1 : -1);
        return;
    }

    if (acDetailActive()) {
        switch (c) {
            case 'w': case 'a': acMove(-1); break;
            case 's': case 'd': acMove(+1); break;
            case 'e': case '=': case '\r': acAction(); break;
            case '0': case 'b':
                if (g_acEditing) {
                    g_acEditing = false;
                    redrawAcRow(1);
                } else {
                    goBack();
                }
                break;
            default:
                if (c >= '1' && c < '1' + AC_ROWS) {
                    int slot = c - '1';
                    if (slot == 5 && acEntity().aux.id.isEmpty()) break;
                    int old = g_acCursor;
                    g_acCursor = slot;
                    g_acEditing = false;
                    redrawAcRow(old);
                    redrawAcRow(slot);
                    sfxArrow();
                }
                break;
        }
        return;
    }

    switch (c) {
        case 'w': case 'a': moveFocus(-1); break;
        case 's': case 'd': moveFocus(+1); break;

        case 'e': primaryAction(); break;
        case '=': case '\r':
            if (cardAt(g_focus).type == EntityType::Climate) openDetail();
            else primaryAction();
            break;
        case '0': case 'b': goBack(); break;
        case '.': openDetail(); break;
        case 'p': changePage(-1); break;
        case 'n': changePage(+1); break;

        default:
            if (c >= '1' && c <= '9') directSelect(c - '1');
            break;
    }
}

// Keep the wake-only decision until release, so holding B to wake cannot
// subsequently open detail or toggle an item during that same press.
static uint8_t g_wakeOnlyButtons = 0;

static void noteButtonActivity() {
    uint8_t pressed = (M5.BtnA.isPressed() ? 1 : 0) |
                      (M5.BtnB.isPressed() ? 2 : 0) |
                      (M5.BtnC.isPressed() ? 4 : 0);
    if (!pressed) return;
    if (g_backlightPhase == BacklightPhase::Off) g_wakeOnlyButtons |= pressed;
    noteActivity();
}

static void handleButtons() {
    static bool aHeld = false, cHeld = false, bHeld = false;

    if (M5.BtnA.wasPressed()) aHeld = !(g_wakeOnlyButtons & 1);
    if (M5.BtnA.wasReleased()) {
        if (aHeld) {
            if (acDetailActive()) {
                if (g_acEditing) acAdjust(-1);
                else acMove(-1);
            } else {
                moveFocus(-1);
            }
        }
        aHeld = false;
        g_wakeOnlyButtons &= ~1;
    }

    if (M5.BtnC.wasPressed()) cHeld = !(g_wakeOnlyButtons & 4);
    if (M5.BtnC.wasReleased()) {
        if (cHeld) {
            if (acDetailActive()) {
                if (g_acEditing) acAdjust(+1);
                else acMove(+1);
            } else {
                moveFocus(+1);
            }
        }
        cHeld = false;
        g_wakeOnlyButtons &= ~4;
    }

    if (M5.BtnB.wasPressed()) bHeld = !(g_wakeOnlyButtons & 2);
    if (bHeld && M5.BtnB.pressedFor(600)) {
        if (g_detail) {
            goBack();
        } else {
            openDetail();
        }
        bHeld = false;
    }
    if (M5.BtnB.wasReleased()) {
        if (bHeld) primaryAction();
        bHeld = false;
        g_wakeOnlyButtons &= ~2;
    }
}

static void handleSerialInput() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (g_cmdMode) {
            if (c == '\r') continue;
            if (c == '\n') {
                appConfigHandleCommand(g_cmdLine);
                g_cmdLine = "";
                g_cmdMode = false;
            } else if (g_cmdLine.length() < 300) {
                g_cmdLine += c;
            }
            continue;
        }

        if (c == ':') {
            g_cmdMode = true;
            g_cmdLine = "";
            Serial.print("> ");
            continue;
        }
        if (c == '\r' || c == '\n') continue;
        if (c == 'S') {
            extern void sendScreenshot();
            sendScreenshot();
            continue;
        }
        handleKey(c);
    }
}

// ---------------------------------------------------------------------
//  State refresh
// ---------------------------------------------------------------------
static void refreshTick() {
    if (!haConfigured() || WiFi.status() != WL_CONNECTED) return;

    uint32_t fullInterval = haStateUpdatesReady() ? 1800000 : 30000;
    if (!g_fullRefreshStarted || millis() - g_lastFullRefresh >= fullInterval) {
        for (int i = 0; i < g_config.entityCount; ++i) g_refreshPending[i] = !g_cards[i].id.isEmpty();
        g_fullRefreshStarted = true;
        g_lastFullRefresh = millis();
        ++g_fullRefreshes;
    }

    if (g_refreshIndex < 0) {
        if ((int32_t)(millis() - g_nextRefresh) < 0) return;
        bool pending = false;
        for (int i = 0; i < g_config.entityCount; ++i) pending |= g_refreshPending[i];
        if (!pending) return;
        g_refreshIndex = 0;
        g_refreshFail = 0;
    }

    while (g_refreshIndex < g_config.entityCount && !g_refreshPending[g_refreshIndex]) ++g_refreshIndex;
    if (g_refreshIndex >= g_config.entityCount) {
        bool anyFail = (g_refreshFail > 0);
        g_refreshIndex = -1;
        g_refreshFail = 0;
        // Retry failed servers quickly, but do not hammer a healthy one.
        g_nextRefresh = millis() + (anyFail ? 15000 : 0);
        return;
    }

    int index = g_refreshIndex++;
    g_refreshPending[index] = false;
    if (index < g_config.entityCount && !g_cards[index].id.isEmpty()) {
        ++g_restRefreshReads;
        bool ok = haFetchState(g_cards[index]);
        if (ok) {
            if (!g_cards[index].customName.isEmpty()) {
                g_cards[index].name = g_cards[index].customName;
            }
            if (!g_cards[index].aux.id.isEmpty()) {
                uint32_t revision = g_cards[index].aux.revision;
                EntityState auxState;
                auxState.id = g_cards[index].aux.id;
                ++g_restRefreshReads;
                bool auxOk = haFetchState(auxState);
                if (!auxOk) {
                    g_refreshPending[index] = true;
                    ++g_refreshFail;
                }
                if (auxOk && g_cards[index].aux.revision == revision) {
                    g_cards[index].aux.name = auxState.name;
                    g_cards[index].aux.state = auxState.state;
                    g_cards[index].aux.unit = auxState.unit;
                    g_cards[index].aux.detail = auxState.detail;
                    g_cards[index].aux.type = auxState.type;
                    g_cards[index].aux.on = auxState.on;
                    g_cards[index].aux.available = auxState.available;
                    g_cards[index].aux.brightness = auxState.brightness;
                    g_cards[index].aux.brightnessAdjustable = auxState.brightnessAdjustable;
                }
            }
            // Cache every page, redraw only rows on the visible page.
            if (index / GRID_SLOTS == g_page) {
                g_dirty[index] = true;
            }
        } else {
            g_refreshPending[index] = true;
            g_refreshFail++;
        }
    }
    // Keep failed and unprocessed rows pending, with bounded retry traffic.
    if (g_refreshFail >= 3) {
        g_refreshIndex = -1;
        g_refreshFail = 0;
        g_nextRefresh = millis() + 15000;
    }
}

// ---------------------------------------------------------------------
//  Screenshot over serial
// ---------------------------------------------------------------------
void sendScreenshot() {
    size_t bytes = SCR_W * SCR_H * 2;
    void* buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!buffer) {
        Serial.printf("SHOT FAIL free=%u max=%u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        return;
    }

    M5Canvas canvas(&M5.Display);
    canvas.setBuffer(buffer, SCR_W, SCR_H, 16);
    renderAll(canvas);

    uint32_t len = bytes;
    uint16_t w = SCR_W;
    uint16_t h = SCR_H;
    Serial.write("SHOTIMG", 7);
    Serial.write((uint8_t*)&w, 2);
    Serial.write((uint8_t*)&h, 2);
    Serial.write((uint8_t*)&len, 4);
    Serial.write((uint8_t*)canvas.getBuffer(), len);
    Serial.write("DONE", 4);
    Serial.flush();

    canvas.deleteSprite();
    heap_caps_free(buffer);
    Serial.println();
    Serial.println("SHOT OK");
}

// ---------------------------------------------------------------------
//  Boot / setup display helpers (called from app_config.cpp)
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
//  Setup / loop
// ---------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(150);
    Serial.println();
    Serial.println("[boot] serial ready");

    configLoad();
    setConnectedPowerIdle(false);
    applyTheme();
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Speaker.setVolume(96);
    quietAudio();
    keypadInit();
    initCommandLeds();

    PowerStatus power = readPowerStatus();
    g_lastBatteryPct = power.batteryPercent;
    g_lastCharging = power.charging == ChargeState::Charging;
    g_lastDockInferred = power.dockInferred;
    g_lastWifiStatus = WiFi.status();

    uint8_t brightness = constrain(g_config.brightness, (uint8_t)1, (uint8_t)100);
    M5.Display.setBrightness((uint8_t)((brightness * 255 + 50) / 100));
    g_commandLedBrightness.store((uint8_t)((brightness * 255 + 50) / 100));

    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_cards[i] = EntityState();
        if (i >= g_config.entityCount) continue;
        g_cards[i].id = g_config.entities[i];
        if (!g_cards[i].id.isEmpty()) {
            g_cards[i].type = entityTypeFromId(g_cards[i].id);
            g_cards[i].customName = g_config.entityNames[i];
            g_cards[i].name = g_cards[i].customName.isEmpty()
                                  ? defaultNameFromId(g_cards[i].id)
                                  : g_cards[i].customName;
            g_cards[i].state = "--";
            if (g_cards[i].type == EntityType::Climate) {
                g_cards[i].aux.id = g_config.auxEntities[i];
            }
            if (!g_cards[i].aux.id.isEmpty()) {
                g_cards[i].aux.type = entityTypeFromId(g_cards[i].aux.id);
                g_cards[i].aux.name = defaultNameFromId(g_cards[i].aux.id);
                g_cards[i].aux.state = "--";
            }
        }
    }

    g_page = 0;
    g_focus = 0;
    g_detailGlobal = -1;
    g_lastActivity = millis();
    g_appliedBrightness = -1;
    g_prevWifiLink = WiFi.status();

    haSetStateCallback(onHaState);
    haSetStatusCallback(onHaStatus);
    haSetAuthCallback(onHaAuth);
    haSetCommandCallback(animateHaCommandLeds);

    // Show the main UI immediately. Networking continues in the background.
    networkBegin();
    renderInitial();

    Serial.printf("[boot] ready bright=%u%% entities=%u pages=%d ha=%s\n",
                  brightness, g_config.entityCount, pageCount(),
                  haConfigured() ? "configured" : "not configured");
}

void loop() {
    M5.update();
    readPowerStatus();
    noteButtonActivity();
    networkLoop();
    handleSerialInput();

    char keypadKey = keypadPoll();
    if (keypadKey) handleKey(keypadKey);

    wl_status_t wifiNow = WiFi.status();
    if (wifiNow == WL_CONNECTED && g_prevWifiLink != WL_CONNECTED) {
        // Wi-Fi is back: resync every page and let HA reconnect.
        g_nextRefresh = 0;
        g_refreshIndex = -1;
        g_fullRefreshStarted = false;
        g_statusLinkDirty = true;
    }
    g_prevWifiLink = wifiNow;

    if (!g_haStarted && wifiNow == WL_CONNECTED && haConfigured()) {
        haBegin();
        g_haStarted = true;
        g_nextRefresh = 0;
        g_refreshIndex = -1;
    }

    handleButtons();
    haLoop();
    scheduleTick();

    static uint32_t lastBlink = 0;
    if (millis() - lastBlink > 500) {
        lastBlink = millis();
        g_linkBlinkOn = !g_linkBlinkOn;
        bool linkUp = (WiFi.status() == WL_CONNECTED) && g_haOnline;
        if (!linkUp) g_statusLinkDirty = true;
    }

    renderDirty();
    refreshTick();
    connectedPowerTick();

    static uint32_t lastBar = 0;
    if (millis() - lastBar > 500) {
        lastBar = millis();

        PowerStatus power = readPowerStatus();
        int pct = power.batteryPercent;
        bool charging = power.charging == ChargeState::Charging;
        if (pct != g_lastBatteryPct || charging != g_lastCharging ||
            power.dockInferred != g_lastDockInferred) {
            g_lastBatteryPct = pct;
            g_lastCharging = charging;
            g_lastDockInferred = power.dockInferred;
            g_statusBatteryDirty = true;
        }

        wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus != g_lastWifiStatus) {
            g_lastWifiStatus = wifiStatus;
            g_statusLinkDirty = true;
        }
    }

    delay(connectedPowerIdle() ? 20 : 10);
}
