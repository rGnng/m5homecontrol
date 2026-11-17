// =====================================================================
//  Home Control - Home Assistant remote for M5Stack Core Basic v2.7
//  3x3 grid, live state via Home Assistant WebSocket API.
// =====================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "app_config.h"
#include "ha_client.h"
#include "sfx_data.h"

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

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int SB_H  = 24;

static constexpr int MENU_Y = 24;
static constexpr int MENU_ROW_H = 27;

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
static wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;
static bool g_dirty[MAX_ENTITIES] = {false};

static int pageCount() {
    return constrain((int)g_config.pageCount, 1, MAX_PAGES);
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
static int g_refreshOk = 0;
static int g_refreshFail = 0;
static uint32_t g_nextRefresh = 0;
static wl_status_t g_prevWifiLink = WL_IDLE_STATUS;

static bool g_cmdMode = false;
static String g_cmdLine;

static void sfxArrow() {
    M5.Speaker.playWav(sfx_arrow, sfx_arrow_len, 1, -1, true);
}

static void sfxSelect() {
    M5.Speaker.playWav(sfx_select, sfx_select_len, 1, -1, true);
}

static uint32_t g_lastActivity = 0;
static bool g_screenOff = false;
static int g_activePeriod = -1;
static int g_appliedBrightness = -1;

// ---------------------------------------------------------------------
//  Text helpers
// ---------------------------------------------------------------------
static void txt(LovyanGFX& g, const char* s, int x, int y,
                uint16_t color, uint8_t size = 2) {
    g.setTextDatum(TL_DATUM);
    g.setTextSize(size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

static void txtC(LovyanGFX& g, const char* s, int x, int y,
                 uint16_t color, uint8_t size = 2) {
    g.setTextDatum(MC_DATUM);
    g.setTextSize(size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

static void txtR(LovyanGFX& g, const char* s, int x, int y,
                 uint16_t color, uint8_t size = 2) {
    g.setTextDatum(TR_DATUM);
    g.setTextSize(size);
    g.setTextColor(color);
    g.drawString(s, x, y);
}

static String shorten(const String& in, size_t maxChars) {
    if (in.length() <= maxChars) return in;
    if (maxChars == 0) return String("");
    return in.substring(0, maxChars - 1) + "~";
}

static String upperText(const String& in) {
    String s = in;
    s.toUpperCase();
    return s;
}

static String tempText(float value) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1fC", value);
    return String(buf);
}

static bool validTemp(float value) {
    return !isnan(value) && value > -100.0f && value < 200.0f;
}

// ---------------------------------------------------------------------
//  Backlight schedule
// ---------------------------------------------------------------------
static void applyBacklightPercent(int percent) {
    percent = constrain(percent, 0, 100);
    int pwm = (percent * 255 + 50) / 100;
    if (pwm != g_appliedBrightness) {
        M5.Display.setBrightness((uint8_t)pwm);
        g_appliedBrightness = pwm;
    }
}

static void noteActivity() {
    g_lastActivity = millis();
    if (g_screenOff) g_screenOff = false;
}

static bool inTimeRange(int now, int start, int end) {
    if (start == end) return false;
    if (start < end) return now >= start && now < end;
    return now >= start || now < end;
}

static void scheduleTick() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0) || timeinfo.tm_year < 120) {
        g_screenOff = false;
        applyBacklightPercent(g_config.brightness);
        return;
    }

    int now = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    bool isDay = inTimeRange(now, g_config.dayStart, g_config.dayEnd);
    bool isNight = inTimeRange(now, g_config.nightStart, g_config.nightEnd);
    int period = isDay ? 0 : (isNight ? 1 : 0);

    uint8_t brightness = (period == 0) ? g_config.dayBrightness
                                       : g_config.nightBrightness;
    uint16_t offSeconds = (period == 0) ? g_config.dayOffSeconds
                                        : g_config.nightOffSeconds;

    if (period != g_activePeriod) {
        g_activePeriod = period;
        g_screenOff = false;
        g_lastActivity = millis();
        applyBacklightPercent(brightness);
        return;
    }

    if (g_screenOff) return;

    if (brightness == 0) {
        applyBacklightPercent(0);
        g_screenOff = true;
        return;
    }

    applyBacklightPercent(brightness);

    if (offSeconds > 0 &&
        millis() - g_lastActivity >= (uint32_t)offSeconds * 1000UL) {
        applyBacklightPercent(0);
        g_screenOff = true;
    }
}

// ---------------------------------------------------------------------
//  Icons
// ---------------------------------------------------------------------
static uint16_t typeAccent(EntityType type) {
    switch (type) {
        case EntityType::Light:   return C_YELLOW;
        case EntityType::Switch:  return C_GREEN;
        case EntityType::Climate: return C_CYAN;
        case EntityType::Scene:   return C_GREEN;
        case EntityType::Media:   return C_MAGENTA;
        case EntityType::Sensor:  return C_ORANGE;
        case EntityType::Cover:   return C_CYAN;
        default:                  return C_DIM;
    }
}

static void iconBulb(LovyanGFX& g, int x, int y, bool on, uint16_t accent) {
    uint16_t c = on ? accent : C_OFF;
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
    uint16_t c = on ? accent : C_OFF;
    int cx = x + 14, cy = y + 14;
    g.drawFastVLine(cx, cy - 11, 22, c);
    g.drawLine(cx - 9, cy - 6, cx + 9, cy + 6, c);
    g.drawLine(cx - 9, cy + 6, cx + 9, cy - 6, c);
    g.drawFastHLine(cx - 4, cy - 11, 8, c);
    g.drawFastHLine(cx - 4, cy + 10, 8, c);
    g.fillCircle(cx, cy, 2, c);
}

static void iconMedia(LovyanGFX& g, int x, int y, bool on, uint16_t accent) {
    uint16_t c = on ? accent : C_OFF;
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
    uint16_t accent = e.available ? typeAccent(e.type) : C_OFF;
    switch (e.type) {
        case EntityType::Light:   iconBulb(g, x, y, e.on, accent); break;
        case EntityType::Switch:  iconPower(g, x, y, e.on ? accent : C_OFF); break;
        case EntityType::Climate: iconClimate(g, x, y, e.on, accent); break;
        case EntityType::Media:   iconMedia(g, x, y, e.on, accent); break;
        case EntityType::Scene:   iconScene(g, x, y, accent); break;
        case EntityType::Sensor:  iconSensor(g, x, y, accent); break;
        case EntityType::Cover:   iconCover(g, x, y, accent); break;
        default:                  iconSensor(g, x, y, C_OFF); break;
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
            float t = validTemp(e.current) ? e.current
                                          : (validTemp(e.target) ? e.target : NAN);
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
            if (!e.unit.isEmpty()) s += " " + e.unit;
            return s;
        }
        case EntityType::Cover:
            return upperText(e.state);
        default:
            return upperText(e.state);
    }
}

static uint16_t entityStateColor(const EntityState& e) {
    if (e.id.isEmpty() || !e.available) return C_OFF;
    switch (e.type) {
        case EntityType::Light:
        case EntityType::Switch:
        case EntityType::Climate:
        case EntityType::Media:
        case EntityType::Cover:
            return e.on ? typeAccent(e.type) : C_OFF;
        default:
            return typeAccent(e.type);
    }
}

static const char* typeLabel(EntityType type) {
    switch (type) {
        case EntityType::Light:   return "LIGHT";
        case EntityType::Switch:  return "SWITCH";
        case EntityType::Climate: return "CLIMATE";
        case EntityType::Scene:   return "SCENE";
        case EntityType::Media:   return "MEDIA";
        case EntityType::Sensor:  return "SENSOR";
        case EntityType::Cover:   return "COVER";
        default:                  return "ENTITY";
    }
}

// ---------------------------------------------------------------------
//  Status bar
// ---------------------------------------------------------------------
static void drawBattery(LovyanGFX& g, int x, int y, int pct, bool charging) {
    // Monochrome battery icon, same color as the signal bars.
    const uint16_t mono = TFT_LIGHTGREY;
    g.drawRect(x, y, 18, 12, mono);
    g.drawRect(x - 1, y - 1, 20, 14, mono);
    g.fillRect(x + 20, y + 4, 2, 5, mono);

    if (charging) {
        g.fillTriangle(x + 10, y + 2, x + 6, y + 6, x + 9, y + 6, mono);
        g.fillTriangle(x + 9, y + 6, x + 6, y + 10, x + 11, y + 6, mono);
        return;
    }

    int fill = (14 * constrain(pct, 0, 100)) / 100;
    if (fill > 0) g.fillRect(x + 2, y + 2, fill, 8, mono);
}

// Combined Wi-Fi + HA link indicator. A red X means either Wi-Fi is down or
// the HA WebSocket is not authenticated.
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
        uint16_t c = (i < bars) ? TFT_LIGHTGREY : C_OFF;
        g.fillRect(x + i * 4, y + 13 - h, 3, h, c);
    }
}

static void drawPageDots(LovyanGFX& g) {
    int pages = pageCount();
    int totalW = pages * 6 + (pages - 1) * 4;
    int startX = 160 - totalW / 2;
    for (int p = 0; p < pages; p++) {
        g.fillRect(startX + p * 10, 9, 6, 6,
                   (p == g_page) ? TFT_WHITE : C_DIM);
    }
}

static void drawStatusTitle(LovyanGFX& g) {
    String title = (g_page < MAX_PAGES) ? g_config.pageTitles[g_page] : String("");
    if (title.isEmpty()) title = "PAGE" + String(g_page + 1);
    title = shorten(title, 7);
    txt(g, title.c_str(), 6, 5, TFT_LIGHTGREY, 2);
}

static void drawStatusCenter(LovyanGFX& g) {
    drawPageDots(g);
}

static void drawStatusBar(LovyanGFX& g) {
    g.fillRect(0, 0, SCR_W, SB_H, C_BAR);
    drawStatusTitle(g);
    drawStatusCenter(g);
    drawLinkIcon(g, 270, 5);
    drawBattery(g, 294, 6, M5.Power.getBatteryLevel(), M5.Power.isCharging());
}

// ---------------------------------------------------------------------
//  Menu list
// ---------------------------------------------------------------------
static void drawMenuRow(LovyanGFX& g, int slot, const EntityState& e, bool selected) {
    int y = MENU_Y + slot * MENU_ROW_H;
    bool empty = e.id.isEmpty();

    g.fillRect(0, y, SCR_W, MENU_ROW_H, selected ? TFT_NAVY : TFT_BLACK);

    uint16_t numberBg = selected ? C_GREEN : (empty ? C_BAR : 0x2104);
    uint16_t numberFg = selected ? TFT_BLACK : (empty ? TFT_BLACK : TFT_WHITE);
    g.fillRect(0, y, 24, MENU_ROW_H, numberBg);

    char number[2] = { char('1' + slot), 0 };
    txt(g, number, 6, y + 5, numberFg, 2);

    // Subtle divider between rows, including the index column.
    if (slot < GRID_SLOTS - 1) {
        g.drawFastHLine(0, y + MENU_ROW_H - 1, SCR_W, C_DIVIDER);
    }

    // Unconfigured slots keep their numbered row but have no label.
    if (empty) return;

    String name = shorten(displayName(e), 12);
    txt(g, name.c_str(), 32, y + 5, selected ? C_GREEN : TFT_WHITE, 2);

    String state = shorten(entityStateText(e), 11);
    txtR(g, state.c_str(), 314, y + 5,
         selected ? C_GREEN : entityStateColor(e), 2);
}

static void drawMenu(LovyanGFX& g) {
    for (int slot = 0; slot < GRID_SLOTS; slot++) {
        drawMenuRow(g, slot, cardAt(slot), slot == g_focus);
    }
}

// ---------------------------------------------------------------------
//  Detail view
// ---------------------------------------------------------------------
static String detailValue(const EntityState& e) {
    if (!e.available) return String("--");
    switch (e.type) {
        case EntityType::Light: {
            if (!e.on) return String("OFF");
            int pct = lightPercent(e);
            return pct >= 0 ? String(pct) + "%" : String("ON");
        }
        case EntityType::Climate: {
            float t = validTemp(e.current) ? e.current
                                          : (validTemp(e.target) ? e.target : NAN);
            return validTemp(t) ? tempText(t) : upperText(e.state);
        }
        case EntityType::Sensor: {
            String s = e.state;
            if (!e.unit.isEmpty()) s += " " + e.unit;
            return shorten(s, 8);
        }
        case EntityType::Media:
            return shorten(upperText(e.state), 7);
        case EntityType::Scene:
            return String("SCENE");
        case EntityType::Switch:
            return e.on ? String("ON") : String("OFF");
        case EntityType::Cover:
            return shorten(upperText(e.state), 7);
        default:
            return String("--");
    }
}

static String detailCaption(const EntityState& e) {
    switch (e.type) {
        case EntityType::Light:   return String("BRIGHTNESS");
        case EntityType::Climate: return String("CURRENT");
        case EntityType::Sensor:  return String("CURRENT");
        case EntityType::Media:   return shorten(e.detail.isEmpty() ? String("MEDIA") : e.detail, 24);
        case EntityType::Scene:   return String("SCENE");
        case EntityType::Switch:  return String("SWITCH");
        case EntityType::Cover:   return String("COVER");
        default:                  return String("VALUE");
    }
}

static void drawDetailHeader(LovyanGFX& g, const EntityState& e) {
    txt(g, typeLabel(e.type), 12, 31, C_DIM, 1);

    String name = shorten(displayName(e), 17);
    txt(g, name.c_str(), 12, 42, C_TEXT, 2);
    txtR(g, shorten(entityStateText(e), 14).c_str(), 308, 44, entityStateColor(e), 1);
}

static void drawDetailPanel(LovyanGFX& g, const EntityState& e) {
    g.fillRect(8, 62, 304, 120, C_CARD);
    g.drawRect(8, 62, 304, 120, C_LINE);
    g.fillRect(8, 62, 4, 120, e.available ? typeAccent(e.type) : C_OFF);

    g.fillRect(22, 80, 48, 48, 0x2104);
    iconFor(g, 32, 90, e);

    txt(g, detailValue(e).c_str(), 88, 72, C_TEXT, 3);
    txt(g, detailCaption(e).c_str(), 88, 102, C_DIM, 1);

    int barPct = -1;
    if (e.type == EntityType::Light) {
        barPct = e.on ? constrain(lightPercent(e), 0, 100) : 0;
    } else if (e.type == EntityType::Climate) {
        float t = validTemp(e.target) ? e.target : 22.0f;
        barPct = (int)(((t - 5.0f) * 100.0f) / 30.0f);
    }
    if (barPct >= 0) {
        barPct = constrain(barPct, 0, 100);
        g.fillRect(88, 118, 200, 10, 0x2945);
        if (barPct > 0) g.fillRect(88, 118, 2 * barPct, 10, typeAccent(e.type));
        g.drawRect(88, 118, 200, 10, C_LINE);
    }

    g.drawFastHLine(22, 140, 276, C_LINE);

    String bottomLeft = shorten(e.id, 30);
    String bottomRight;
    if (e.type == EntityType::Climate) {
        float t = validTemp(e.target) ? e.target : NAN;
        bottomRight = validTemp(t) ? ("TARGET " + tempText(t)) : String("TARGET --");
    } else if (e.type == EntityType::Media) {
        bottomRight = shorten(e.detail, 20);
    } else if (e.type == EntityType::Light) {
        bottomRight = e.on ? String("STATE ON") : String("STATE OFF");
    } else if (e.type == EntityType::Sensor) {
        bottomRight = shorten(e.detail, 20);
    }
    txt(g, bottomLeft.c_str(), 22, 150, C_DIM, 1);
    txtR(g, bottomRight.c_str(), 304, 150, C_DIM, 1);
}

static void drawDetail(LovyanGFX& g, const EntityState& e) {
    drawDetailHeader(g, e);
    drawDetailPanel(g, e);
}

// ---------------------------------------------------------------------
//  Render
// ---------------------------------------------------------------------
static void renderAll(LovyanGFX& g) {
    g.fillScreen(C_BG);
    drawStatusBar(g);

    if (g_detail && g_detailGlobal >= 0) {
        drawDetail(g, g_cards[g_detailGlobal]);
    } else {
        drawMenu(g);
    }
}

// Full initial draw only. Runtime updates use the partial redraw helpers.
static void renderInitial() {
    M5.Display.startWrite();
    renderAll(M5.Display);
    M5.Display.endWrite();
}

static void redrawContent() {
    M5.Display.startWrite();
    M5.Display.fillRect(0, SB_H, SCR_W, SCR_H - SB_H, C_BG);
    if (g_detail && g_detailGlobal >= 0) {
        drawDetail(M5.Display, g_cards[g_detailGlobal]);
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

static void redrawDetail() {
    if (g_detailGlobal < 0 || g_detailGlobal >= g_config.entityCount) return;
    M5.Display.startWrite();
    M5.Display.fillRect(0, SB_H, SCR_W, 39, C_BG);
    drawDetailHeader(M5.Display, g_cards[g_detailGlobal]);
    drawDetailPanel(M5.Display, g_cards[g_detailGlobal]);
    M5.Display.endWrite();
}

static void redrawRow(int slot, bool selected) {
    M5.Display.startWrite();
    drawMenuRow(M5.Display, slot, cardAt(slot), selected);
    M5.Display.endWrite();
}

static void redrawStatusRegions() {
    if (!g_statusTitleDirty && !g_statusCenterDirty &&
        !g_statusLinkDirty && !g_statusBatteryDirty) {
        return;
    }

    M5.Display.startWrite();
    if (g_statusTitleDirty) {
        M5.Display.fillRect(0, 0, 96, SB_H, C_BAR);
        drawStatusTitle(M5.Display);
    }
    if (g_statusCenterDirty) {
        M5.Display.fillRect(92, 0, 144, SB_H, C_BAR);
        drawStatusCenter(M5.Display);
    }
    if (g_statusLinkDirty) {
        M5.Display.fillRect(264, 0, 26, SB_H, C_BAR);
        drawLinkIcon(M5.Display, 270, 5);
    }
    if (g_statusBatteryDirty) {
        M5.Display.fillRect(290, 0, 30, SB_H, C_BAR);
        drawBattery(M5.Display, 294, 6, M5.Power.getBatteryLevel(),
                    M5.Power.isCharging());
    }
    M5.Display.endWrite();

    g_statusTitleDirty = false;
    g_statusCenterDirty = false;
    g_statusLinkDirty = false;
    g_statusBatteryDirty = false;
}

static void renderDirty() {
    if (g_detail) {
        bool detailDirty = (g_detailGlobal >= 0 && g_dirty[g_detailGlobal]);
        for (int i = 0; i < MAX_ENTITIES; i++) g_dirty[i] = false;
        if (detailDirty) redrawDetail();
        redrawStatusRegions();
        return;
    }

    M5.Display.startWrite();
    for (int slot = 0; slot < GRID_SLOTS; slot++) {
        int index = globalSlot(slot);
        if (index >= g_config.entityCount) continue;
        if (!g_dirty[index]) continue;
        g_dirty[index] = false;
        drawMenuRow(M5.Display, slot, g_cards[index], slot == g_focus);
    }
    M5.Display.endWrite();

    redrawStatusRegions();
}

// ---------------------------------------------------------------------
//  WebSocket callbacks
// ---------------------------------------------------------------------
static void onHaState(const EntityState& state) {
    for (int i = 0; i < g_config.entityCount; i++) {
        if (g_cards[i].id == state.id) {
            String custom = g_cards[i].customName;
            g_cards[i] = state;
            g_cards[i].customName = custom;
            if (!custom.isEmpty()) g_cards[i].name = custom;
            if (i / GRID_SLOTS == g_page || (g_detail && i == g_detailGlobal)) {
                g_dirty[i] = true;
            }
            if (!g_haOnline) {
                g_haOnline = true;
                g_statusLinkDirty = true;
            }
            return;
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
    g_refreshOk = 0;
    g_nextRefresh = 0;
}

// ---------------------------------------------------------------------
//  Actions
// ---------------------------------------------------------------------
static void scheduleRefresh() {
    g_refreshIndex = -1;
    g_nextRefresh = millis() + 1500;
}

static void goBack() {
    if (g_detail) {
        g_detail = false;
        redrawContent();
        sfxArrow();
    }
}

static void openDetail() {
    if (g_detail) return;
    if (cardAt(g_focus).id.isEmpty()) return;
    g_detail = true;
    g_detailGlobal = globalSlot(g_focus);
    redrawContent();
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
    if (g_haOnline != ok) {
        g_haOnline = ok;
        g_statusLinkDirty = true;
    }
    return ok;
}

static void primaryAction() {
    if (g_detail) {
        goBack();
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
        case EntityType::Sensor:
        default:
            openDetail();
            return;
    }

    if (ok) scheduleRefresh();
    if (g_detail) {
        redrawDetail();
    } else {
        redrawRow(g_focus, true);
    }
    redrawStatusRegions();
}

static void adjustSelected(int delta) {
    EntityState& e = selectedEntity();
    if (e.id.isEmpty()) return;

    if (e.type == EntityType::Light) {
        int pct = lightPercent(e);
        if (pct < 0) pct = e.on ? 50 : 0;
        pct = constrain(pct + delta * 10, 0, 100);
        bool ok = callService(entityDomain(e.id), "turn_on", e.id,
                              "\"brightness_pct\":" + String(pct));
        if (ok) {
            e.on = pct > 0;
            e.state = e.on ? "on" : "off";
            e.brightness = (pct * 255 + 50) / 100;
            scheduleRefresh();
        }
    } else if (e.type == EntityType::Climate) {
        float target = validTemp(e.target) ? e.target : 22.0f;
        target = constrain(target + delta * 0.5f, 5.0f, 35.0f);
        char extra[40];
        snprintf(extra, sizeof(extra), "\"temperature\":%.1f", target);
        bool ok = callService(entityDomain(e.id), "set_temperature", e.id, extra);
        if (ok) {
            e.target = target;
            e.on = true;
            scheduleRefresh();
        }
    } else {
        return;
    }
    if (g_detail) {
        redrawDetail();
    } else {
        redrawRow(g_focus, true);
    }
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
    noteActivity();
    switch (c) {
        case 'w': case 'a': moveFocus(-1); break;
        case 's': case 'd': moveFocus(+1); break;

        case 'e': case '=': case '\r': primaryAction(); break;
        case '0': case 'b': case 'A': goBack(); break;
        case 'M': case '.': openDetail(); break;
        case 'p': changePage(-1); break;
        case 'n': changePage(+1); break;
        case '+': adjustSelected(+1); break;
        case '-': adjustSelected(-1); break;

        default:
            if (c >= '1' && c <= '9') directSelect(c - '1');
            break;
    }
}

static void handleButtons() {
    static bool aHeld = false, cHeld = false, bHeld = false;

    if (M5.BtnA.wasPressed()) aHeld = true;
    if (aHeld && M5.BtnA.pressedFor(600)) {
        if (!g_detail) changePage(-1);
        aHeld = false;
    }
    if (M5.BtnA.wasReleased()) {
        if (aHeld) {
            if (g_detail) adjustSelected(-1);
            else moveFocus(-1);
        }
        aHeld = false;
    }

    if (M5.BtnC.wasPressed()) cHeld = true;
    if (cHeld && M5.BtnC.pressedFor(600)) {
        if (!g_detail) changePage(+1);
        cHeld = false;
    }
    if (M5.BtnC.wasReleased()) {
        if (cHeld) {
            if (g_detail) adjustSelected(+1);
            else moveFocus(+1);
        }
        cHeld = false;
    }

    if (M5.BtnB.wasPressed()) bHeld = true;
    if (bHeld && M5.BtnB.pressedFor(600)) {
        if (!g_detail) openDetail();
        bHeld = false;
    }
    if (M5.BtnB.wasReleased()) {
        if (bHeld) primaryAction();
        bHeld = false;
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

    if (g_refreshIndex < 0) {
        if ((int32_t)(millis() - g_nextRefresh) < 0) return;
        g_refreshIndex = 0;
        g_refreshOk = 0;
        g_refreshFail = 0;
    }

    if (g_refreshIndex >= g_config.entityCount) {
        bool anyOk = (g_refreshOk > 0);
        bool anyFail = (g_refreshFail > 0);
        g_refreshIndex = -1;
        g_refreshOk = 0;
        g_refreshFail = 0;
        // Retry failed servers quickly, but do not hammer a healthy one.
        g_nextRefresh = millis() + (anyFail ? 15000 : 300000);
        bool online = anyOk || haWsConnected();
        if (g_haOnline != online) {
            g_haOnline = online;
            g_statusLinkDirty = true;
        }
        return;
    }

    int index = g_refreshIndex++;
    if (index < g_config.entityCount && !g_cards[index].id.isEmpty()) {
        bool ok = haFetchState(g_cards[index]);
        if (ok) {
            g_refreshOk++;
            if (!g_haOnline) {
                g_haOnline = true;
                g_statusLinkDirty = true;
            }
            if (!g_cards[index].customName.isEmpty()) {
                g_cards[index].name = g_cards[index].customName;
            }
            // Cache every page, redraw only rows on the visible page.
            if (index / GRID_SLOTS == g_page) {
                g_dirty[index] = true;
            }
        } else {
            g_refreshFail++;
            // Abort this sweep after three timeouts instead of blocking the
            // UI for every entity. It will retry forever every 15 seconds.
            if (g_refreshFail >= 3) {
                g_refreshIndex = -1;
                g_refreshOk = 0;
                g_refreshFail = 0;
                g_nextRefresh = millis() + 15000;
                bool online = haWsConnected();
                if (g_haOnline != online) {
                    g_haOnline = online;
                    g_statusLinkDirty = true;
                }
            }
        }
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
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Speaker.setVolume(96);

    g_lastBatteryPct = M5.Power.getBatteryLevel();
    g_lastCharging = M5.Power.isCharging();
    g_lastWifiStatus = WiFi.status();

    uint8_t brightness = constrain(g_config.brightness, (uint8_t)1, (uint8_t)100);
    M5.Display.setBrightness((uint8_t)((brightness * 255 + 50) / 100));

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

    // Show the main UI immediately. Networking continues in the background.
    networkBegin();
    renderInitial();

    Serial.printf("[boot] ready bright=%u%% entities=%u pages=%d ha=%s\n",
                  brightness, g_config.entityCount, pageCount(),
                  haConfigured() ? "configured" : "not configured");
}

void loop() {
    M5.update();
    if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
        noteActivity();
    }
    networkLoop();
    handleSerialInput();

    wl_status_t wifiNow = WiFi.status();
    if (wifiNow == WL_CONNECTED && g_prevWifiLink != WL_CONNECTED) {
        // Wi-Fi is back: resync every page and let HA reconnect.
        g_nextRefresh = 0;
        g_refreshIndex = -1;
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

    static uint32_t lastBar = 0;
    if (millis() - lastBar > 5000) {
        lastBar = millis();

        int pct = M5.Power.getBatteryLevel();
        bool charging = M5.Power.isCharging();
        if (pct != g_lastBatteryPct || charging != g_lastCharging) {
            g_lastBatteryPct = pct;
            g_lastCharging = charging;
            g_statusBatteryDirty = true;
        }

        wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus != g_lastWifiStatus) {
            g_lastWifiStatus = wifiStatus;
            g_statusLinkDirty = true;
        }
    }

    delay(10);
}
