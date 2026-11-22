#include "app_config.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_system.h>

#include "ha_client.h"
#include "connected_power.h"
#include "html_stream.h"
#include "power_status.h"

static const char* NVS_NAMESPACE = "haremote";
static const char* DEFAULT_HA_URL = "";
static const char* DEFAULT_HOSTNAME = "ha-remote";

AppConfig g_config;

static WebServer server(80);
static bool g_apMode = false;
static bool g_routesReady = false;
static bool g_serverStarted = false;
static bool g_staEverConnected = false;
static bool g_mdnsStarted = false;
static uint32_t g_connectStart = 0;
static uint32_t g_lastReconnect = 0;

static String sanitizeHostname(const String& value) {
    String in = value;
    in.trim();
    if (in.endsWith(".local")) in = in.substring(0, in.length() - 6);
    String out;
    out.reserve(32);
    for (size_t i = 0; i < in.length() && out.length() < 32; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        if (isalnum((unsigned char)c)) {
            out += c;
        } else if (c == '-' && out.length() > 0 && !out.endsWith("-")) {
            out += '-';
        }
    }
    while (out.endsWith("-")) out.remove(out.length() - 1);
    if (out.isEmpty()) out = DEFAULT_HOSTNAME;
    return out;
}

// ---------------------------------------------------------------------
//  NVS
// ---------------------------------------------------------------------
static void normalizeAuxEntities() {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (!g_config.entities[i].startsWith("climate.")) {
            g_config.auxEntities[i] = "";
        }
        if (g_config.auxEntities[i].isEmpty()) g_config.auxNames[i] = "";
    }
}

void configLoad() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    g_config.ssid = p.getString("ssid", "");
    g_config.pass = p.getString("pass", "");
    g_config.hostname = sanitizeHostname(p.getString("host", DEFAULT_HOSTNAME));
    g_config.haUrl = p.getString("url", DEFAULT_HA_URL);
    g_config.haToken = p.getString("token", "");
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_config.entities[i] = p.getString(("e" + String(i)).c_str(), "");
        g_config.entityNames[i] = p.getString(("n" + String(i)).c_str(), "");
        g_config.auxEntities[i] = p.getString(("x" + String(i)).c_str(), "");
        g_config.auxNames[i] = p.getString(("y" + String(i)).c_str(), "");
        int mask = p.getInt(("cm" + String(i)).c_str(), -1);
        if (mask < 0) {
            mask = g_config.entities[i].startsWith("climate.")
                       ? (CLIMATE_COOL | CLIMATE_HEAT | CLIMATE_DRY)
                       : 0;
        }
        g_config.climateModes[i] = (uint8_t)(mask & 0x1F);
    }
    int pages = p.getInt("pages", -1);
    if (pages < 0) {
        int count = p.getInt("count", -1);
        if (count < 0) {
            count = 0;
            for (int i = 0; i < MAX_ENTITIES; i++) {
                if (!g_config.entities[i].isEmpty()) count = i + 1;
            }
        }
        pages = (constrain(count, 0, MAX_ENTITIES) + GRID_SLOTS - 1) / GRID_SLOTS;
        if (pages < 1) pages = 1;
    }
    g_config.pageCount = (uint8_t)constrain(pages, 1, MAX_PAGES);
    g_config.entityCount = (uint8_t)(g_config.pageCount * GRID_SLOTS);
    for (int i = g_config.entityCount; i < MAX_ENTITIES; i++) {
        g_config.entities[i] = "";
        g_config.entityNames[i] = "";
        g_config.auxEntities[i] = "";
        g_config.auxNames[i] = "";
        g_config.climateModes[i] = 0;
    }
    for (int page = 0; page < MAX_PAGES; page++) {
        g_config.pageTitles[page] =
            p.getString(("t" + String(page)).c_str(), "");
        if (g_config.pageTitles[page].isEmpty()) {
            g_config.pageTitles[page] = "PAGE" + String(page + 1);
        }
    }
    int bright = p.getInt("bright", 30);
    if (bright < 1 || bright > 100) bright = 30;
    g_config.brightness = (uint8_t)bright;
    g_config.theme = (uint8_t)constrain(p.getInt("theme", 0), 0, 1);

    g_config.dayStart = constrain(p.getInt("ds", 7 * 60), 0, 1439);
    g_config.dayEnd = constrain(p.getInt("de", 21 * 60), 0, 1439);
    g_config.dayBrightness = constrain(p.getInt("db", 5), 0, 100);
    g_config.dayDimBrightness = constrain(p.getInt("ddb", 1), 1, 100);
    g_config.dayDimSeconds = constrain(p.getInt("dd", 0), 0, 3600);
    g_config.dayOffSeconds = constrain(p.getInt("do", 0), 0, 3600);

    g_config.nightStart = constrain(p.getInt("ns", 21 * 60), 0, 1439);
    g_config.nightEnd = constrain(p.getInt("ne", 7 * 60), 0, 1439);
    g_config.nightBrightness = constrain(p.getInt("nb", 5), 0, 100);
    g_config.nightDimBrightness = constrain(p.getInt("ndb", 1), 1, 100);
    // Migrate the old single off timer. Preserve a custom nonzero timeout;
    // an old disabled timer adopts the new 10 min dim + 2 min off defaults.
    bool hasDimSchedule = p.isKey("nd");
    int legacyOff = constrain(p.getInt("no", 0), 0, 3600);
    g_config.nightDimSeconds = constrain(p.getInt("nd", legacyOff ? 0 : 600), 0, 3600);
    g_config.nightOffSeconds = hasDimSchedule ? legacyOff : (legacyOff ? legacyOff : 120);
    p.end();
    normalizeAuxEntities();
}

bool configSave() {
    normalizeAuxEntities();
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;

    bool ok = true;
    auto putStringKey = [&](const char* key, const String& value) {
        if (value.isEmpty()) {
            if (p.isKey(key)) p.remove(key);
        } else if (p.putString(key, value) == 0) {
            ok = false;
        }
    };
    auto putIntKey = [&](const char* key, int value) {
        if (p.putInt(key, value) == 0) ok = false;
    };

    putStringKey("ssid", g_config.ssid);
    putStringKey("pass", g_config.pass);
    putStringKey("host", g_config.hostname);
    putStringKey("url", g_config.haUrl);
    putStringKey("token", g_config.haToken);
    putIntKey("pages", g_config.pageCount);
    putIntKey("count", g_config.entityCount);
    putIntKey("bright", g_config.brightness);
    putIntKey("theme", g_config.theme);

    // Only persist existing slots. Empty slots are removed so the small NVS
    // partition does not fill with hundreds of empty keys.
    for (int i = 0; i < g_config.entityCount; i++) {
        putStringKey(("e" + String(i)).c_str(), g_config.entities[i]);
        putStringKey(("n" + String(i)).c_str(), g_config.entityNames[i]);
        putStringKey(("x" + String(i)).c_str(), g_config.auxEntities[i]);
        putStringKey(("y" + String(i)).c_str(), g_config.auxNames[i]);
        putIntKey(("cm" + String(i)).c_str(), g_config.climateModes[i]);
    }
    for (int i = g_config.entityCount; i < MAX_ENTITIES; i++) {
        const char* keys[] = {"e", "n", "x", "y", "cm"};
        for (const char* prefix : keys) {
            String key = String(prefix) + String(i);
            if (p.isKey(key.c_str())) p.remove(key.c_str());
        }
    }
    for (int page = 0; page < g_config.pageCount; page++) {
        putStringKey(("t" + String(page)).c_str(), g_config.pageTitles[page]);
    }
    for (int page = g_config.pageCount; page < MAX_PAGES; page++) {
        String tk = "t" + String(page);
        if (p.isKey(tk.c_str())) p.remove(tk.c_str());
    }

    putIntKey("ds", g_config.dayStart);
    putIntKey("de", g_config.dayEnd);
    putIntKey("db", g_config.dayBrightness);
    putIntKey("ddb", g_config.dayDimBrightness);
    putIntKey("dd", g_config.dayDimSeconds);
    putIntKey("do", g_config.dayOffSeconds);
    putIntKey("ns", g_config.nightStart);
    putIntKey("ne", g_config.nightEnd);
    putIntKey("nb", g_config.nightBrightness);
    putIntKey("ndb", g_config.nightDimBrightness);
    putIntKey("nd", g_config.nightDimSeconds);
    putIntKey("no", g_config.nightOffSeconds);
    p.end();
    return ok;
}

bool configComplete() {
    return !g_config.haUrl.isEmpty() && !g_config.haToken.isEmpty();
}

bool haConfigured() {
    return configComplete();
}

// ---------------------------------------------------------------------
//  HTML helpers
// ---------------------------------------------------------------------
static String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static String formatTime(int minutes) {
    minutes = constrain(minutes, 0, 1439);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return String(buf);
}

static int parseTime(const String& value, int fallback) {
    int colon = value.indexOf(':');
    if (colon < 0) return fallback;
    int hh = value.substring(0, colon).toInt();
    int mm = value.substring(colon + 1).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return fallback;
    return hh * 60 + mm;
}

static String htmlHeader(const String& title) {
    String h;
    h.reserve(512);
    h += "<!doctype html><html><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    h += "<title>" + title + "</title><style>";
    h += "html{color-scheme:dark}";
    h += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;";
    h += "max-width:1400px;margin:24px auto;padding:0 16px;color:#ddd;background:#000}";
    h += "h1{color:#fff;font-size:22px;margin:0 0 12px}p{color:#999;line-height:1.4}";
    h += ".layout{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.35fr);gap:24px;align-items:start}";
    h += ".col{min-width:0}";
    h += "fieldset{min-width:0;border:1px solid #333;margin:0 0 18px;padding:12px 14px;background:#080808}";
    h += "legend{font-weight:600;padding:0 4px;color:#fff}label{display:block;margin:10px 0 4px;font-size:14px;color:#bbb}";
    h += "button,.button-link,input,select{box-sizing:border-box;height:36px;font:inherit;font-size:14px;font-weight:400;line-height:20px}";
    h += "input,select{width:100%;padding:7px 8px;border:1px solid #444;border-radius:0;background:#111;color:#eee}";
    h += "input:focus,select:focus{outline:none;border-color:#aaa}";
    h += "button,.button-link{display:inline-flex;align-items:center;justify-content:center;margin:0;padding:7px 12px;border:1px solid #666;border-radius:0;background:#111;color:#eee;text-decoration:none;white-space:nowrap;cursor:pointer}";
    h += "button[type=submit]{background:#eee;color:#000;border-color:#eee}";
    h += ".power-row{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:16px}";
    h += ".battery-icon{display:block;width:60px;height:auto;color:#ddd}";
    h += ".connection-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:9px 0}";
    h += ".connection-row+.connection-row{border-top:1px solid #222}.connection-info{min-width:0}";
    h += ".connection-name{display:block;color:#ddd;font-size:14px}.connection-value{display:block;color:#888;font-size:12px;margin-top:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".connection-state{flex-shrink:0;font-size:12px;color:#999}.connection-state:before{content:'';display:inline-block;width:6px;height:6px;border-radius:50%;background:currentColor;margin-right:6px}";
    h += ".connection-state.up{color:#92c5a4}.connection-state.down{color:#d99898}";
    h += ".metadata{display:grid;grid-template-columns:auto minmax(0,1fr);gap:5px 12px;font-size:12px;color:#888}.metadata dd{margin:0;color:#bbb;overflow-wrap:anywhere}";
    h += ".connection-clock{margin:8px 0 0;padding-top:10px;border-top:1px solid #222}";
    h += ".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:20}";
    h += ".modal.open{display:block}";
    h += ".modal-body{background:#0a0a0a;color:#ddd;max-width:640px;max-height:80vh;overflow:auto;margin:10vh auto;padding:18px;border:1px solid #444}";
    h += ".modal-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin:0 0 12px}";
    h += "table{width:100%;border-collapse:collapse}td{padding:4px 0;vertical-align:top}";
    h += "td.n{width:28px;color:#777;padding-top:12px}code{background:#1a1a1a;color:#eee;padding:1px 4px}";
    h += "th{text-align:left;font-size:12px;color:#888;font-weight:600;padding:4px 0}";
    h += ".modesrow td{padding:0 0 10px}.modelabel{font-size:12px;color:#888;margin-right:8px}";
    h += ".auxfields{display:grid;grid-template-columns:1fr 1fr;gap:8px}";
    h += ".schedule-period{margin:12px 0 16px}.schedule-title{font-weight:600;color:#ddd}";
    h += ".schedule-line{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin-top:8px}";
    h += ".schedule-times{grid-template-columns:repeat(2,minmax(0,1fr))}";
    h += ".schedule-line label{min-width:0;margin:0;font-size:12px;display:flex;flex-direction:column;justify-content:flex-end}";
    h += ".schedule-line input{min-width:0;margin-top:5px;padding:7px 5px}";
    h += "label.inline{display:inline-flex;align-items:center;gap:4px;margin:0 10px 0 0;font-size:12px;color:#aaa}";
    h += "label.inline input{width:auto;height:auto;margin:0}";
    h += ".pagehead{font-weight:600;margin:18px 0 6px;color:#fff}";
    h += ".pagebar{display:flex;align-items:center;gap:6px;margin:18px 0 6px}";
    h += ".page-number{min-width:2ch;text-align:right;color:#888;font-size:14px}";
    h += ".pagebar .ptitle{flex:1;min-width:0;width:0}";
    h += ".pagebar button{flex-shrink:0}";
    h += "button:disabled{opacity:.35;cursor:default}";
    h += ".actions{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin:12px 0 0}";
    h += ".display-preview button[aria-pressed=true]{background:#eee;color:#000;border-color:#eee}";
    h += ".display-preview button[data-error=true]{border-color:#d77}";
    h += ".actions.top{flex-wrap:nowrap;justify-content:space-between;margin:0 0 16px}";
    h += ".actions.top h1{margin:0;white-space:nowrap}.actions.top button{flex-shrink:0;white-space:nowrap}";
    h += ".footer-actions{justify-content:flex-end;margin:16px 0 0}";
    h += ".device-info{margin:12px 0 0}";
    h += ".device-actions{margin:0}";
    h += ".save-status{color:#bbb;font-size:14px;margin:0 0 16px}.save-status:empty{display:none}";
    h += "@media(max-width:800px){.layout{grid-template-columns:1fr}}";
    h += "@media(max-width:400px){.actions.top h1{font-size:20px}}";
    h += "</style></head><body>";
    return h;
}

static String htmlFooter() {
    return "</body></html>";
}

static void sendMessagePage(const String& title, const String& message) {
    String page = htmlHeader(title);
    page += "<h1>" + htmlEscape(title) + "</h1><p>" + htmlEscape(message) + "</p>";
    page += htmlFooter();
    server.send(200, "text/html", page);
}

static String powerSummary() {
    PowerStatus power = readPowerStatus(true);
    String text = "Battery: ";
    text += power.batteryPercent < 0 ? String("Unavailable")
                                    : String(power.batteryPercent) + "% (approximate)";
    return text;
}

static String powerIcon() {
    PowerStatus power = readPowerStatus(true);
    int percent = power.batteryPercent;
    bool known = percent >= 0 && percent <= 100;
    String label = known ? "Battery: " + String(percent) + "% (approximate)"
                         : String("Battery status unavailable");
    String svg = "<svg class=\"battery-icon\" role=\"img\" aria-label=\"" + label +
                 "\" viewBox=\"0 0 46 24\" xmlns=\"http://www.w3.org/2000/svg\"><title>" + label + "</title>";
    svg += "<rect x=\"1\" y=\"2\" width=\"38\" height=\"20\" rx=\"1\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
           "<rect x=\"38\" y=\"8\" width=\"6\" height=\"8\" fill=\"currentColor\"/>";
    if (known) {
        int width = (percent * 32 + 50) / 100;
        if (percent > 0 && width == 0) width = 1;
        svg += "<rect class=\"battery-level\" x=\"4\" y=\"5\" width=\"" + String(width) +
               "\" height=\"14\" fill=\"currentColor\"/>";
    } else {
        svg += "<text x=\"20\" y=\"18\" text-anchor=\"middle\" font-size=\"18\" fill=\"currentColor\">?</text>";
    }
    return svg + "</svg>";
}

static void handleRoot() {
    ConnectedPowerWork powerWork;
    static String cachedOptions;
    static uint32_t cachedAt = 0;
    bool optionsLoaded = false;

    if (!g_apMode && WiFi.status() == WL_CONNECTED && !g_config.haToken.isEmpty()) {
        if (cachedOptions.isEmpty() || millis() - cachedAt > 60000) {
            cachedOptions = "";
            if (haFetchEntityOptions(cachedOptions, 300)) {
                cachedAt = millis();
            }
        }
        optionsLoaded = !cachedOptions.isEmpty();
    }

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    HtmlStream<WebServer> page(server, haLoop);
    page += htmlHeader("HA Remote Setup");

    bool wifiConnected = !g_apMode && WiFi.status() == WL_CONNECTED;
    String wifiLabel = g_apMode ? String("HA-Remote")
                               : htmlEscape(wifiConnected ? WiFi.SSID() : g_config.ssid);
    String haLabel;
    String haState;
    if (g_config.haToken.isEmpty()) {
        haLabel = "Not configured";
        haState = "idle";
    } else if (wifiConnected && haWsConnected()) {
        haLabel = "Connected";
        haState = "up";
    } else {
        haLabel = "Disconnected";
        haState = "down";
    }

    page += "<form method=\"post\" action=\"/save\">";
    page += "<div class=\"actions top\"><h1>HA Remote</h1>"
            "<button type=\"submit\">Save and reboot</button></div>";
    page += "<div id=\"save-status\" class=\"save-status\" role=\"status\"></div>";
    page += "<div class=\"layout\">";
    page += "<div class=\"col left\">";

    page += "<fieldset><legend>Connection</legend>";
    auto connectionRow = [&](const char* label, const String& value,
                             const String& state, const String& stateClass) {
        page += "<div class=\"connection-row\"><div class=\"connection-info\">";
        page += "<span class=\"connection-name\">" + String(label) + "</span>";
        page += "<span class=\"connection-value\" title=\"" + value + "\">" + value + "</span></div>";
        page += "<span class=\"connection-state " + stateClass + "\">" + state + "</span></div>";
    };
    connectionRow("Wi-Fi", wifiLabel, g_apMode ? "Access point" : (wifiConnected ? "Connected" : "Disconnected"),
                  wifiConnected ? "up" : (g_apMode ? "idle" : "down"));
    connectionRow("Home Assistant", htmlEscape(g_config.haUrl), haLabel, haState);
    page += "<dl class=\"metadata connection-clock\"><dt>Time zone</dt><dd>" + htmlEscape(haTimeZone()) +
            "</dd><dt>Time</dt><dd>" + htmlEscape(haLocalTimeText()) + "</dd></dl>";
    page += "<div style=\"margin-top:12px\">";
    page += "<button type=\"button\" class=\"secondary\" onclick=\"openConnection()\">Edit connection</button>";
    page += "</div></fieldset>";

    page += "<fieldset><legend>Power</legend><div class=\"power-row\">";
    page += "<span id=\"power-status\" role=\"status\">" + powerIcon() + "</span>";
    page += "<button type=\"button\" class=\"secondary\" onclick=\"refreshPower()\">Refresh</button>";
    page += "</div></fieldset>";

    page += "<fieldset><legend>Display</legend>";
    page += "<label>Theme</label><select name=\"theme\">";
    page += "<option value=\"flipper\"" +
            String(g_config.theme == 0 ? " selected" : "") + ">Flipper</option>";
    page += "<option value=\"classic\"" +
            String(g_config.theme == 1 ? " selected" : "") + ">Classic</option>";
    page += "</select>";
    page += "<label>Fallback brightness (%)</label><input name=\"bright\" type=\"number\" min=\"1\" max=\"100\" value=\"" +
            String(g_config.brightness) + "\">";
    page += "<div class=\"pagehead\">Backlight Schedule</div>";
    auto scheduleRow = [&](const char* label, const char* prefix, uint16_t start, uint16_t end,
                           uint8_t brightness, uint8_t dimBrightness, uint16_t dim, uint16_t off) {
        page += "<div class=\"schedule-period\"><div class=\"schedule-title\">" + String(label) + "</div>";
        page += "<div class=\"schedule-line schedule-times\">";
        page += "<label>Start<input name=\"" + String(prefix) + "start\" type=\"time\" value=\"" + formatTime(start) + "\"></label>";
        page += "<label>End<input name=\"" + String(prefix) + "end\" type=\"time\" value=\"" + formatTime(end) + "\"></label>";
        auto number = [&](const char* title, const char* suffix, unsigned value, unsigned min, unsigned max) {
            page += "<label>" + String(title) + "<input name=\"" + String(prefix) + suffix + "\" type=\"number\" min=\"" +
                    String(min) + "\" max=\"" + String(max) + "\" value=\"" + String(value) + "\"></label>";
        };
        page += "</div><div class=\"schedule-line\">";
        number("Brightness %", "bright", brightness, 0, 100);
        number("Dim %", "dimbright", dimBrightness, 1, 100);
        number("Dim after (s)", "dim", dim, 0, 3600);
        number("Off after dim (s)", "off", off, 0, 3600);
        page += "</div></div>";
    };
    scheduleRow("Day", "d", g_config.dayStart, g_config.dayEnd, g_config.dayBrightness,
                g_config.dayDimBrightness, g_config.dayDimSeconds, g_config.dayOffSeconds);
    scheduleRow("Night", "n", g_config.nightStart, g_config.nightEnd, g_config.nightBrightness,
                g_config.nightDimBrightness, g_config.nightDimSeconds, g_config.nightOffSeconds);
    page += "<p>0 = disabled. Without dimming, Off counts from last input.</p>";
    page += "<div class=\"pagehead\">Preview display</div>";
    page += "<div class=\"actions display-preview\" role=\"group\" aria-label=\"Display preview\">"
            "<button type=\"button\" data-display-mode=\"wake\" aria-pressed=\"false\" onclick=\"previewDisplay('wake')\">Wake</button>"
            "<button type=\"button\" data-display-mode=\"dim\" aria-pressed=\"false\" onclick=\"previewDisplay('dim')\">Dim</button>"
            "<button type=\"button\" data-display-mode=\"off\" aria-pressed=\"false\" onclick=\"previewDisplay('off')\">Off</button></div>";
    page += "</fieldset>";

    page += "<fieldset><legend>Device</legend>";
    page += "<div class=\"actions device-actions\">"
            "<a class=\"button-link\" href=\"/update\">Update</a>"
            "<a class=\"button-link\" href=\"/screenshot.bmp\" download=\"ha-remote-screen.bmp\">Screenshot</a></div>";
    page += "<dl class=\"metadata device-info\"><dt>Serial baud</dt><dd>115200</dd></dl>";
    page += "</fieldset>";

    page += "</div>";
    page += "<div class=\"col right\">";
    page += "<fieldset><legend>Entity List</legend>";

    if (optionsLoaded) {
        page += "<datalist id=\"entity-list\">";
        page += cachedOptions;
        page += "</datalist>";
    } else if (g_config.haToken.isEmpty()) {
        page += "<p>Configure the Home Assistant token to load the entity selector. "
                "Manual entity IDs also work.</p>";
    } else {
        page += "<p>Could not load entities from Home Assistant. Manual entity IDs still work.</p>";
    }

    page += "<input type=\"hidden\" name=\"pagecount\" id=\"pagecount\" value=\"" +
            String(g_config.pageCount) + "\">";
    page += "<div id=\"pages\">";
    for (int p = 0; p < g_config.pageCount; p++) {
        page += "<div class=\"pageblock\">";
        page += "<div class=\"pagebar\"><span class=\"page-number\" aria-label=\"Page number\">" + String(p + 1) +
                "</span><input class=\"ptitle\" aria-label=\"Page title\" placeholder=\"Page title\" name=\"t" + String(p) +
                "\" value=\"" + htmlEscape(g_config.pageTitles[p]) + "\">";
        page += "<button type=\"button\" class=\"move-up\" aria-label=\"Move page up\" title=\"Move page up\" onclick=\"movePage(this,-1)\">&uarr;</button>"
                "<button type=\"button\" class=\"move-down\" aria-label=\"Move page down\" title=\"Move page down\" onclick=\"movePage(this,1)\">&darr;</button>"
                "<button type=\"button\" class=\"remove\" onclick=\"removePage(this)\">Remove</button></div>";
        page += "<table><tbody>";
        page += "<tr><th></th><th>Entity ID</th><th>Display name</th></tr>";
        for (int slot = 0; slot < GRID_SLOTS; slot++) {
            int index = p * GRID_SLOTS + slot;
            String value = index < g_config.entityCount
                               ? g_config.entities[index]
                               : String("");
            String name = index < g_config.entityCount
                              ? g_config.entityNames[index]
                              : String("");
            String aux = index < g_config.entityCount
                             ? g_config.auxEntities[index]
                             : String("");
            String auxName = index < g_config.entityCount
                                 ? g_config.auxNames[index]
                                 : String("");
            uint8_t mask = index < g_config.entityCount
                               ? g_config.climateModes[index]
                               : 0;
            bool climate = value.startsWith("climate.");

            page += "<tr class=\"slotrow\"><td class=\"n\">" + String(slot + 1) + "</td><td>";
            page += "<input class=\"entity\" name=\"e" + String(index) +
                    "\" list=\"entity-list\" value=\"" + htmlEscape(value) +
                    "\" placeholder=\"light.living_room\">";
            page += "</td><td>";
            page += "<input class=\"ename\" name=\"n" + String(index) +
                    "\" value=\"" + htmlEscape(name) +
                    "\" placeholder=\"optional name\">";
            page += "</td></tr>";

            page += "<tr class=\"modesrow\" style=\"display:" +
                    String(climate ? "" : "none") + "\"><td></td><td colspan=\"2\">";
            page += "<span class=\"modelabel\">MODES</span>";
            const char* codes[] = {"c", "h", "d", "f", "a"};
            const char* labels[] = {"COOL", "HEAT", "DRY", "FAN", "AUTO"};
            const uint8_t bits[] = {CLIMATE_COOL, CLIMATE_HEAT, CLIMATE_DRY,
                                    CLIMATE_FAN, CLIMATE_AUTO};
            for (int m = 0; m < 5; m++) {
                page += "<label class=\"inline\"><input type=\"checkbox\" data-code=\"" +
                        String(codes[m]) + "\" name=\"m" + String(index) + codes[m] +
                        "\"" + ((mask & bits[m]) ? " checked" : "") + "> " +
                        labels[m] + "</label>";
            }
            page += "<div class=\"auxfields\"><label>AUX entity";
            page += "<input class=\"auxentity\" name=\"x" + String(index) +
                    "\" list=\"entity-list\" value=\"" + htmlEscape(aux) +
                    "\" placeholder=\"optional entity\"" + String(climate ? "" : " disabled") + ">";
            page += "</label><label>AUX name";
            page += "<input class=\"auxname\" name=\"y" + String(index) +
                    "\" value=\"" + htmlEscape(auxName) +
                    "\" placeholder=\"optional name\"" + String(climate ? "" : " disabled") + ">";
            page += "</label></div></td></tr>";
        }
        page += "</tbody></table></div>";
    }
    page += "</div>";
    page += "<div class=\"actions\"><button type=\"button\" id=\"add-page\" onclick=\"addPage()\">Add page</button></div>";
    page += "</fieldset>";
    page += "</div>";
    page += "</div>";

    page += "<div class=\"modal\" id=\"connection-modal\">";
    page += "<div class=\"modal-body\">";
    page += "<div class=\"pagehead modal-heading\"><span>Connection</span> "
            "<button type=\"button\" class=\"remove\" onclick=\"closeConnection()\">Close</button></div>";
    page += "<label>Hostname (.local)</label><input name=\"host\" autocomplete=\"off\" value=\"" +
            htmlEscape(g_config.hostname) + "\" placeholder=\"ha-remote\">";
    page += "<label>Wi-Fi SSID</label><input name=\"ssid\" autocomplete=\"off\" value=\"" +
            htmlEscape(g_config.ssid) + "\">";
    page += "<label>Wi-Fi password (leave blank to keep the saved password)</label>";
    page += "<input name=\"pass\" type=\"password\" autocomplete=\"off\">";
    page += "<label>Home Assistant base URL</label><input name=\"url\" value=\"" +
            htmlEscape(g_config.haUrl) + "\" placeholder=\"http://&lt;ha-host&gt;:8123\">";
    page += "<label>Long-lived access token (leave blank to keep the saved token)</label>";
    page += "<input name=\"token\" type=\"password\" autocomplete=\"off\" placeholder=\"";
    page += g_config.haToken.isEmpty() ? "not configured" : "configured";
    page += "\">";
    page += "<p>The token is stored in NVS and never shown again.</p>";
    page += "<div class=\"actions\"><button type=\"submit\">Save and reboot</button></div>";
    page += "</div></div>";

    page += R"JS(
<script>
const MAX_PAGES = )JS";
    page += String(MAX_PAGES);
    page += R"JS(;
const SLOTS = )JS";
    page += String(GRID_SLOTS);
    page += R"JS(;
function updateModesRow(row) {
  const entity = row.querySelector('.entity').value.trim();
  const climate = entity.toLowerCase().startsWith('climate.');
  const modesRow = row.nextElementSibling;
  if (modesRow && modesRow.classList.contains('modesrow')) {
    modesRow.style.display = climate ? '' : 'none';
    modesRow.querySelectorAll('input').forEach(function(input) {
      input.disabled = !climate;
    });
  }
}
function bindRows() {
  document.querySelectorAll('#pages .slotrow').forEach(function(row) {
    const input = row.querySelector('.entity');
    if (!input.dataset.bound) {
      input.dataset.bound = '1';
      input.addEventListener('input', function() { updateModesRow(row); });
    }
    updateModesRow(row);
  });
}
function renumberPages() {
  const blocks = document.querySelectorAll('#pages .pageblock');
  blocks.forEach(function(block, p) {
    block.querySelector('.page-number').textContent = p + 1;
    block.querySelector('.move-up').disabled = p === 0;
    block.querySelector('.move-down').disabled = p === blocks.length - 1;
    block.querySelector('.remove').disabled = blocks.length <= 1;
    block.querySelector('.ptitle').name = 't' + p;
    block.querySelectorAll('.slotrow').forEach(function(row, slot) {
      const idx = p * SLOTS + slot;
      row.querySelector('.entity').name = 'e' + idx;
      row.querySelector('.ename').name = 'n' + idx;
      const modesRow = row.nextElementSibling;
      if (modesRow && modesRow.classList.contains('modesrow')) {
        modesRow.querySelector('.auxentity').name = 'x' + idx;
        modesRow.querySelector('.auxname').name = 'y' + idx;
        modesRow.querySelectorAll('input[type=checkbox]').forEach(function(cb) {
          cb.name = 'm' + idx + cb.dataset.code;
        });
      }
    });
  });
  document.getElementById('pagecount').value = blocks.length;
  document.getElementById('add-page').disabled = blocks.length >= MAX_PAGES;
  bindRows();
}
function addPage() {
  const blocks = document.querySelectorAll('#pages .pageblock');
  if (blocks.length >= MAX_PAGES) return;
  let rows = '';
  for (let slot = 0; slot < SLOTS; slot++) {
    rows += '<tr class="slotrow"><td class="n">' + (slot + 1) + '</td>' +
      '<td><input class="entity" list="entity-list" placeholder="light.living_room"></td>' +
      '<td><input class="ename" placeholder="optional name"></td></tr>' +
      '<tr class="modesrow" style="display:none"><td></td><td colspan="2">' +
      '<span class="modelabel">MODES</span>' +
      '<label class="inline"><input type="checkbox" data-code="c" checked> COOL</label>' +
      '<label class="inline"><input type="checkbox" data-code="h" checked> HEAT</label>' +
      '<label class="inline"><input type="checkbox" data-code="d" checked> DRY</label>' +
      '<label class="inline"><input type="checkbox" data-code="f"> FAN</label>' +
      '<label class="inline"><input type="checkbox" data-code="a"> AUTO</label>' +
      '<div class="auxfields"><label>AUX entity<input class="auxentity" list="entity-list" placeholder="optional entity" disabled></label>' +
      '<label>AUX name<input class="auxname" placeholder="optional name" disabled></label></div>' +
      '</td></tr>';
  }
  const block = document.createElement('div');
  block.className = 'pageblock';
  block.innerHTML = '<div class="pagebar"><span class="page-number" aria-label="Page number"></span>' +
    '<input class="ptitle" aria-label="Page title" placeholder="Page title" value="PAGE' + (blocks.length + 1) + '">' +
    '<button type="button" class="move-up" aria-label="Move page up" title="Move page up" onclick="movePage(this,-1)">&uarr;</button>' +
    '<button type="button" class="move-down" aria-label="Move page down" title="Move page down" onclick="movePage(this,1)">&darr;</button>' +
    '<button type="button" class="remove" onclick="removePage(this)">Remove</button></div>' +
    '<table><tbody><tr><th></th><th>Entity ID</th><th>Display name</th></tr>' + rows + '</tbody></table>';
  document.getElementById('pages').appendChild(block);
  renumberPages();
}
function movePage(button, direction) {
  const block = button.closest('.pageblock');
  const sibling = direction < 0 ? block.previousElementSibling : block.nextElementSibling;
  if (!sibling) return;
  if (direction < 0) block.parentNode.insertBefore(block, sibling);
  else block.parentNode.insertBefore(sibling, block);
  renumberPages();
}
function removePage(button) {
  const blocks = document.querySelectorAll('#pages .pageblock');
  if (blocks.length <= 1) return;
  button.closest('.pageblock').remove();
  renumberPages();
}
renumberPages();
function openConnection() {
  document.getElementById('connection-modal').classList.add('open');
}
async function refreshPower() {
  const status = document.getElementById('power-status');
  try {
    const response = await fetch('/power?format=icon', {cache: 'no-store'});
    if (!response.ok) throw new Error('Power status unavailable');
    status.innerHTML = await response.text();
    status.removeAttribute('title');
  } catch (error) {
    const message = 'Could not refresh battery status. Retry.';
    status.title = message;
    const icon = status.querySelector('svg');
    if (icon) {
      icon.setAttribute('aria-label', message);
      const title = icon.querySelector('title');
      if (title) title.textContent = message;
    }
  }
}
async function previewDisplay(mode) {
  const buttons = [...document.querySelectorAll('.display-preview button')];
  buttons.forEach(button => {
    button.disabled = true;
    button.removeAttribute('data-error');
    button.removeAttribute('title');
  });
  try {
    const response = await fetch('/display', {
      method: 'POST', body: new URLSearchParams({mode})
    });
    if (!response.ok) throw new Error('Display control failed');
    buttons.forEach(button => button.setAttribute('aria-pressed', String(button.dataset.displayMode === mode)));
  } catch (error) {
    const button = buttons.find(button => button.dataset.displayMode === mode);
    if (button) {
      button.dataset.error = 'true';
      button.title = 'Could not apply display preview. Retry.';
    }
  } finally {
    buttons.forEach(button => button.disabled = false);
  }
}
async function reconnectAfterSave() {
  // Give the save handler time to finish its response and restart the device.
  await new Promise(resolve => setTimeout(resolve, 1500));
  for (let attempt = 0; attempt < 60; attempt++) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 2000);
    try {
      const response = await fetch('/diag', {cache: 'no-store', signal: controller.signal});
      if (response.ok) {
        window.location.reload();
        return;
      }
    } catch (error) {} finally { clearTimeout(timer); }
    await new Promise(resolve => setTimeout(resolve, 1000));
  }
  throw new Error('Settings saved, but reconnecting timed out. You can retry or reload the page.');
}
function closeConnection() {
  document.getElementById('connection-modal').classList.remove('open');
}
document.getElementById('connection-modal').addEventListener('click', function(event) {
  if (event.target === this) closeConnection();
});
document.addEventListener('keydown', function(event) {
  if (event.key === 'Escape') closeConnection();
});
document.querySelector('form[action="/save"]').addEventListener('submit', function(event) {
  event.preventDefault();
  const form = this;
  const status = document.getElementById('save-status');
  const buttons = form.querySelectorAll('button[type="submit"]');
  buttons.forEach(function(button) { button.disabled = true; });
  status.textContent = 'Saving...';
  let saved = false;
  const payload = new URLSearchParams(new FormData(form));
  payload.set('ajax', '1');
  fetch('/save', {
    method: 'POST',
    body: payload
  }).then(async function(response) {
    const text = await response.text();
    if (!response.ok) throw new Error(text || 'Save failed');
    saved = true;
    status.textContent = 'Saved. Rebooting...';
    await reconnectAfterSave();
  }).catch(function(error) {
    status.textContent = saved ? error.message : 'Save failed: ' + error.message;
    buttons.forEach(function(button) { button.disabled = false; });
  });
});
</script>
)JS";

    page += "<div class=\"actions footer-actions\"><button type=\"submit\">Save and reboot</button></div>";
    page += "</form>";
    page += htmlFooter();

    page.finish();
}

static void sendSaveError(const String& message) {
    if (server.arg("ajax") == "1") {
        server.send(400, "application/json",
                    "{\"ok\":false,\"message\":\"" + message + "\"}");
    } else {
        sendMessagePage("Error", message);
    }
}

static void handleSave() {
    ConnectedPowerWork powerWork;
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String hostname = sanitizeHostname(server.arg("host"));
    String url = server.arg("url");
    String token = server.arg("token");
    String brightArg = server.arg("bright");

    ssid.trim();
    url.trim();
    if (ssid.isEmpty()) {
        sendSaveError("SSID is required.");
        return;
    }
    if (url.isEmpty()) {
        sendSaveError("Home Assistant URL is required.");
        return;
    }
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        url = "http://" + url;
    }
    while (url.endsWith("/")) {
        url.remove(url.length() - 1);
    }

    g_config.ssid = ssid;
    if (!pass.isEmpty()) g_config.pass = pass;
    g_config.hostname = hostname;
    g_config.haUrl = url;
    if (!token.isEmpty()) g_config.haToken = token;

    int bright = brightArg.toInt();
    if (bright < 1) bright = 30;
    if (bright > 100) bright = 100;
    g_config.brightness = (uint8_t)bright;
    g_config.theme = server.arg("theme") == "classic" ? 1 : 0;

    g_config.dayStart = parseTime(server.arg("dstart"), g_config.dayStart);
    g_config.dayEnd = parseTime(server.arg("dend"), g_config.dayEnd);
    g_config.dayBrightness = constrain(server.arg("dbright").toInt(), 0, 100);
    if (server.hasArg("ddimbright")) g_config.dayDimBrightness = constrain(server.arg("ddimbright").toInt(), 1, 100);
    if (server.hasArg("ddim")) g_config.dayDimSeconds = constrain(server.arg("ddim").toInt(), 0, 3600);
    g_config.dayOffSeconds = constrain(server.arg("doff").toInt(), 0, 3600);

    g_config.nightStart = parseTime(server.arg("nstart"), g_config.nightStart);
    g_config.nightEnd = parseTime(server.arg("nend"), g_config.nightEnd);
    g_config.nightBrightness = constrain(server.arg("nbright").toInt(), 0, 100);
    if (server.hasArg("ndimbright")) g_config.nightDimBrightness = constrain(server.arg("ndimbright").toInt(), 1, 100);
    if (server.hasArg("ndim")) g_config.nightDimSeconds = constrain(server.arg("ndim").toInt(), 0, 3600);
    g_config.nightOffSeconds = constrain(server.arg("noff").toInt(), 0, 3600);

    int pages = server.arg("pagecount").toInt();
    pages = constrain(pages, 1, MAX_PAGES);
    g_config.pageCount = (uint8_t)pages;
    g_config.entityCount = (uint8_t)(pages * GRID_SLOTS);

    for (int p = 0; p < MAX_PAGES; p++) {
        String title;
        if (p < pages) title = server.arg("t" + String(p));
        title.trim();
        if (title.isEmpty()) title = "PAGE" + String(p + 1);
        g_config.pageTitles[p] = title;
    }

    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (i < g_config.entityCount) {
            String value = server.arg("e" + String(i));
            value.trim();
            value.toLowerCase();
            g_config.entities[i] = value;

            String name = server.arg("n" + String(i));
            name.trim();
            g_config.entityNames[i] = name;

            String aux = server.arg("x" + String(i));
            aux.trim();
            aux.toLowerCase();
            g_config.auxEntities[i] = aux;

            String auxName = server.arg("y" + String(i));
            auxName.trim();
            g_config.auxNames[i] = aux.isEmpty() ? String("") : auxName;

            uint8_t mask = 0;
            if (value.startsWith("climate.")) {
                if (server.hasArg("m" + String(i) + "c")) mask |= CLIMATE_COOL;
                if (server.hasArg("m" + String(i) + "h")) mask |= CLIMATE_HEAT;
                if (server.hasArg("m" + String(i) + "d")) mask |= CLIMATE_DRY;
                if (server.hasArg("m" + String(i) + "f")) mask |= CLIMATE_FAN;
                if (server.hasArg("m" + String(i) + "a")) mask |= CLIMATE_AUTO;
                if (mask == 0) mask = CLIMATE_COOL | CLIMATE_HEAT | CLIMATE_DRY;
            }
            g_config.climateModes[i] = mask;
        } else {
            g_config.entities[i] = "";
            g_config.entityNames[i] = "";
            g_config.auxEntities[i] = "";
            g_config.auxNames[i] = "";
            g_config.climateModes[i] = 0;
        }
    }

    Serial.printf("[web] save pages=%d t0=%s e0=%s bright=%d\n",
                  pages, g_config.pageTitles[0].c_str(),
                  g_config.entities[0].c_str(), bright);

    if (!configSave()) {
        Serial.println("[web] NVS save FAILED");
        sendSaveError("Failed to write NVS.");
        return;
    }
    Serial.println("[web] NVS save OK");

    if (server.arg("ajax") == "1") {
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        String page = htmlHeader("Saved");
        page += "<h1>Saved</h1><p>The device is rebooting with the new configuration.</p>";
        page += htmlFooter();
        server.send(200, "text/html", page);
    }
    delay(900);
    Serial.flush();
    ESP.restart();
}

// Only the owning HTTP upload may abort or finish the shared Update object.
struct WebFirmwareUpload {
    bool ownsUpdate = false;
    bool fileEnded = false;
    String error;
};
static WebFirmwareUpload g_firmwareUpload;

static bool firmwareAuthenticated() {
    return server.authenticate(FIRMWARE_UPDATE_USER, FIRMWARE_UPDATE_PASSWORD);
}

static void handleFirmwarePage() {
    if (!firmwareAuthenticated()) {
        server.requestAuthentication(BASIC_AUTH, "Firmware update");
        return;
    }
    String page = htmlHeader("Update firmware");
    page += R"HTML(
<h1>Update firmware</h1>
<p>Choose the Home Control firmware.bin for this device. Settings are kept.
The device restarts after a successful update.</p>
<form id="update-form" method="post" action="/update" enctype="multipart/form-data">
<label for="firmware">Firmware (.bin)</label>
<input id="firmware" type="file" name="firmware" accept=".bin" required>
<div class="actions"><button id="upload" type="submit">Upload and restart</button></div>
</form>
<p id="update-status" role="status" aria-live="polite"></p>
<p><a href="/">Back to configuration</a></p>
<script>
document.getElementById('update-form').addEventListener('submit', function(event) {
  event.preventDefault();
  const file = document.getElementById('firmware').files[0];
  if (!file || !file.size) return;
  const button = document.getElementById('upload');
  const status = document.getElementById('update-status');
  button.disabled = true;
  status.textContent = 'Uploading...';
  const data = new FormData();
  data.append('firmware', file);
  const request = new XMLHttpRequest();
  request.open('POST', '/update?size=' + file.size);
  request.upload.onprogress = function(event) {
    if (event.lengthComputable) {
      status.textContent = 'Uploading: ' + Math.round(event.loaded * 100 / event.total) + '%';
    }
  };
  request.onload = function() {
    status.textContent = request.responseText;
    if (request.status !== 200) button.disabled = false;
  };
  request.onerror = function() {
    status.textContent = 'Connection lost. Check the device before retrying.';
    button.disabled = false;
  };
  request.send(data);
});
</script>
)HTML";
    page += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", page);
}

static void failFirmwareUpload(const String& error) {
    if (g_firmwareUpload.ownsUpdate) Update.abort();
    g_firmwareUpload.ownsUpdate = false;
    g_firmwareUpload.error = error;
    Serial.printf("[web-ota] %s\n", error.c_str());
}

static bool g_firmwarePowerWork = false;

static void finishFirmwarePowerWork() {
    if (!g_firmwarePowerWork) return;
    g_firmwarePowerWork = false;
    endConnectedPowerWork();
}

static void handleFirmwareUpload() {
    // Upload callbacks run before the final POST handler. Authenticate here
    // as well so rejected requests never write to flash.
    if (!firmwareAuthenticated()) return;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        // WebServer processes the upload in this main-loop invocation. Keep
        // full CPU speed until the request completes, including between chunks.
        setConnectedPowerIdle(false);
        if (!g_firmwarePowerWork) {
            beginConnectedPowerWork();
            g_firmwarePowerWork = true;
        }
        if (g_firmwareUpload.ownsUpdate || g_firmwareUpload.fileEnded) {
            failFirmwareUpload("Upload exactly one firmware file.");
            return;
        }
        if (!g_firmwareUpload.error.isEmpty()) return;
        String sizeArg = server.arg("size");
        bool validSize = !sizeArg.isEmpty() && sizeArg.length() <= 10;
        for (size_t i = 0; i < sizeArg.length(); ++i) {
            if (sizeArg[i] < '0' || sizeArg[i] > '9') validSize = false;
        }
        unsigned long size = strtoul(sizeArg.c_str(), nullptr, 10);
        if (upload.name != "firmware" || !validSize || !size ||
            size > ESP.getFreeSketchSpace()) {
            failFirmwareUpload("Choose one firmware file with a valid size that fits the OTA partition.");
        } else if (Update.isRunning()) {
            failFirmwareUpload("Another firmware update is running.");
        } else if (!Update.begin(size, U_FLASH)) {
            failFirmwareUpload(Update.errorString());
        } else {
            g_firmwareUpload.ownsUpdate = true;
            Serial.printf("[web-ota] receiving %lu bytes\n", size);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE && g_firmwareUpload.ownsUpdate) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            failFirmwareUpload(Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_END && g_firmwareUpload.ownsUpdate) {
        g_firmwareUpload.fileEnded = true;
        if (upload.totalSize != Update.size()) {
            failFirmwareUpload("Firmware size does not match the upload size.");
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        failFirmwareUpload("Upload interrupted.");
        g_firmwareUpload = WebFirmwareUpload();
        finishFirmwarePowerWork();
    }
}

static void handleFirmwareComplete() {
    finishFirmwarePowerWork();
    if (!firmwareAuthenticated()) {
        server.requestAuthentication(BASIC_AUTH, "Firmware update");
        return;
    }
    bool ok = g_firmwareUpload.ownsUpdate && g_firmwareUpload.fileEnded &&
              g_firmwareUpload.error.isEmpty();
    if (ok && !Update.end()) {
        failFirmwareUpload(Update.errorString());
        ok = false;
    }
    if (!ok && g_firmwareUpload.error.isEmpty()) {
        failFirmwareUpload("No complete firmware file received.");
    }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Connection", "close");
    server.send(ok ? 200 : 400, "text/plain", ok
                    ? String("Firmware updated. Restarting; reconnect in a few seconds.")
                    : "Update failed: " + g_firmwareUpload.error);
    g_firmwareUpload = WebFirmwareUpload();
    if (ok) {
        Serial.println("[web-ota] complete; restarting");
        delay(500);
        server.client().stop();
        ESP.restart();
    }
}

static void handleScreenshot() {
    ConnectedPowerWork powerWork;
    bool restoreSleep = beginDisplayRead();
    struct RestorePanel {
        bool sleep;
        ~RestorePanel() { endDisplayRead(sleep); }
    } restore{restoreSleep};
    // Read cached panel pixels without navigating or lighting the backlight.
    constexpr int width = 320, height = 240;
    constexpr size_t rowBytes = width * 3;  // 960 bytes; already a multiple of 4.
    constexpr size_t imageBytes = rowBytes * height;
    uint8_t header[54] = {'B', 'M'};
    auto littleEndian = [&header](size_t offset, uint32_t value) {
        for (size_t i=0; i<4; ++i) { header[offset+i] = value & 0xff; value >>= 8; }
    };
    littleEndian(2, sizeof(header) + imageBytes);
    littleEndian(10, sizeof(header));
    littleEndian(14, 40);  // BITMAPINFOHEADER.
    littleEndian(18, width);
    littleEndian(22, height);
    header[26] = 1;   // One plane.
    header[28] = 24;  // Uncompressed BGR, 8 bits per channel.
    littleEndian(34, imageBytes);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", "attachment; filename=\"ha-remote-screen.bmp\"");
    server.setContentLength(sizeof(header) + imageBytes);
    server.send(200, "image/bmp", "");
    auto client = server.client();
    if (client.write(header, sizeof(header)) != sizeof(header)) {
        client.stop();
        return;
    }
    uint8_t row[rowBytes];
    // BMP stores the bottom row first and reverses RGB to BGR.
    for (int y=height-1; y>=0; --y) {
        M5.Display.readRectRGB(0, y, width, 1, row);
        for (size_t x=0; x<rowBytes; x+=3) {
            uint8_t red = row[x]; row[x] = row[x+2]; row[x+2] = red;
        }
        if (client.write(row, sizeof(row)) != sizeof(row)) {
            client.stop();
            return;
        }
    }
}

static void startWebServer() {
    if (!g_routesReady) {
        server.on("/", HTTP_GET, handleRoot);
        server.on("/save", HTTP_POST, handleSave);
        server.on("/screenshot.bmp", HTTP_GET, handleScreenshot);
        server.on("/power", HTTP_GET, []() {
            server.sendHeader("Cache-Control", "no-store");
            if (server.arg("format") == "icon") server.send(200, "image/svg+xml", powerIcon());
            else server.send(200, "text/plain", powerSummary());
        });
        server.on("/display", HTTP_POST, []() {
            if (!setDisplayMode(server.arg("mode"))) {
                server.send(400, "text/plain", "Unknown display mode");
                return;
            }
            server.sendHeader("Cache-Control", "no-store");
            server.send(200, "text/plain", "OK");
        });
        server.on("/update", HTTP_GET, handleFirmwarePage);
        server.on("/update", HTTP_POST, handleFirmwareComplete, handleFirmwareUpload);
        server.on("/diag", HTTP_GET, []() {
            PowerStatus power = readPowerStatus();
            String text = "board=" + String((int)M5.getBoard());
            text += " uptime_ms=" + String(millis());
            text += " reset_reason=" + String(static_cast<int>(esp_reset_reason()));
            text += " free_heap=" + String(ESP.getFreeHeap());
            text += " min_free_heap=" + String(ESP.getMinFreeHeap());
            text += " " + menuStatus();
            text += " " + backlightStatus();
            text += " " + refreshStatus();
            text += " " + haClockStatus();
            text += " " + haConnectionStatus();
            text += " " + connectedPowerStatus();
            text += " " + commandLedsStatus();
            text += " charging=" + String(chargeStateName(power.charging));
            text += " core_charging=" + String(chargeStateName(power.coreCharging));
            text += " core_input=" + String(power.coreInput);
            text += " dock_charging=unknown dock_status_available=0";
            text += " dock_inferred=" + String((int)power.dockInferred);
            text += " pmic_online=" + String((int)power.pmicOnline);
            text += " core_keep_on=" + String(power.coreKeepOn);
            text += " core_boost_enabled=" + String(power.coreBoostEnabled);
            text += " core_charge_enabled=" + String(power.coreChargeEnabled);
            text += " core_battery_handoff=" + String(power.coreBatteryHandoff);
            text += " power_recoveries=" + String(power.recoveryAttempts);
            text += " vbus=" + String(power.inputMillivolts);
            text += " battery_mv=" + String(power.batteryMillivolts);
            text += " battery=" + String(power.batteryPercent);
            text += " i2c_sda=" + String(M5.In_I2C.getSDA());
            text += " i2c_scl=" + String(M5.In_I2C.getSCL());
            if (M5.getBoard() == m5::board_t::board_M5Stack) {
                for (uint8_t reg : {0x00, 0x01, 0x70, 0x71, 0x78}) {
                    uint8_t value = 0;
                    text += " ip5306_";
                    if (reg < 0x10) text += "0";
                    text += String(reg, HEX) + "=";
                    if (M5.Power.Ip5306.readRegister(reg, &value, 1)) {
                        text += String(value);
                    } else {
                        text += "na";
                    }
                }
            }
            server.send(200, "text/plain", text);
        });
        server.onNotFound([]() {
            server.sendHeader("Location", "/");
            server.send(302, "text/plain", "");
        });
        g_routesReady = true;
    }
    if (!g_serverStarted) {
        server.begin();
        g_serverStarted = true;
    }
}

// ---------------------------------------------------------------------
//  Network
// ---------------------------------------------------------------------
static void startAccessPoint() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("HA-Remote");
    g_apMode = true;
    startWebServer();
    Serial.printf("[net] AP mode ssid=HA-Remote ip=%s\n", WiFi.softAPIP().toString().c_str());
}

void networkBegin() {
    if (g_config.ssid.isEmpty()) {
        startAccessPoint();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);  // WIFI_PS_MIN_MODEM: retain association and HA connection.
    WiFi.setHostname(g_config.hostname.c_str());
    WiFi.begin(g_config.ssid.c_str(), g_config.pass.c_str());
    g_apMode = false;
    g_connectStart = millis();
    g_lastReconnect = millis();
    Serial.printf("[net] connecting ssid=%s\n", g_config.ssid.c_str());
}

void networkLoop() {
    if (g_serverStarted) server.handleClient();

    if (g_apMode) return;

    if (WiFi.status() == WL_CONNECTED) {
        if (!g_staEverConnected) {
            g_staEverConnected = true;
            g_connectStart = 0;
            Serial.printf("[net] station ssid=%s ip=%s rssi=%d\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
            if (!g_mdnsStarted && MDNS.begin(g_config.hostname.c_str())) {
                MDNS.addService("http", "tcp", 80);
                g_mdnsStarted = true;
            }
            startWebServer();
        }
        return;
    }

    // Never give up. This covers wrong credentials, missing AP, and DHCP
    // failure (associated but no IP). Retry forever every 10 seconds.
    if (millis() - g_lastReconnect > 15000) {
        g_lastReconnect = millis();
        Serial.printf("[net] retry WiFi status=%d\n", (int)WiFi.status());
        WiFi.disconnect();
        WiFi.begin(g_config.ssid.c_str(), g_config.pass.c_str());
    }
}

bool configModeActive() {
    return g_apMode;
}

String networkIp() {
    return g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

// ---------------------------------------------------------------------
//  Serial console
// ---------------------------------------------------------------------
static void printConfig() {
    Serial.printf("[cfg] hostname=%s.local\n", g_config.hostname.c_str());
    Serial.printf("[cfg] wifi ssid=%s pass=%s\n",
                  g_config.ssid.c_str(), g_config.pass.isEmpty() ? "(none)" : "(saved)");
    Serial.printf("[cfg] ha url=%s token=%s\n",
                  g_config.haUrl.c_str(),
                  g_config.haToken.isEmpty() ? "(none)"
                                             : (g_config.haToken.length() > 8
                                                    ? (g_config.haToken.substring(0, 4) + "..." +
                                                       g_config.haToken.substring(g_config.haToken.length() - 4)).c_str()
                                                    : "********"));
    Serial.printf("[cfg] fallback brightness=%u%%\n", g_config.brightness);
    Serial.printf("[cfg] day %s-%s bright=%u%% dim=%u%% after=%us off=%us later\n",
                  formatTime(g_config.dayStart).c_str(),
                  formatTime(g_config.dayEnd).c_str(),
                  g_config.dayBrightness, g_config.dayDimBrightness,
                  g_config.dayDimSeconds, g_config.dayOffSeconds);
    Serial.printf("[cfg] night %s-%s bright=%u%% dim=%u%% after=%us off=%us later\n",
                  formatTime(g_config.nightStart).c_str(),
                  formatTime(g_config.nightEnd).c_str(),
                  g_config.nightBrightness, g_config.nightDimBrightness,
                  g_config.nightDimSeconds, g_config.nightOffSeconds);
    Serial.printf("[cfg] pages=%u\n", g_config.pageCount);
    for (int p = 0; p < g_config.pageCount; p++) {
        Serial.printf("[cfg] page %d title=%s\n", p + 1,
                      g_config.pageTitles[p].c_str());
    }
    Serial.printf("[cfg] slots=%u\n", g_config.entityCount);
    for (int i = 0; i < g_config.entityCount; i++) {
        if (g_config.entities[i].isEmpty()) continue;
        if (g_config.entityNames[i].isEmpty()) {
            Serial.printf("[cfg] entity %d = %s\n", i + 1, g_config.entities[i].c_str());
        } else {
            Serial.printf("[cfg] entity %d = %s name=%s\n", i + 1,
                          g_config.entities[i].c_str(),
                          g_config.entityNames[i].c_str());
        }
    }
    Serial.printf("[cfg] ip=%s mode=%s\n", networkIp().c_str(), g_apMode ? "AP" : "STA");
}

static void rebootSoon(const char* message) {
    Serial.println(message);
    Serial.flush();
    delay(300);
    ESP.restart();
}

void appConfigHandleCommand(const String& raw) {
    String line = raw;
    line.trim();
    if (line.isEmpty()) return;

    String lower = line;
    lower.toLowerCase();

    if (lower.startsWith("wifi ")) {
        int sp = line.indexOf(' ');
        String ssid = line.substring(sp + 1);
        ssid.trim();
        int sp2 = ssid.indexOf(' ');
        if (sp2 < 0) {
            Serial.println("[cfg] usage: :wifi <ssid> <password>");
            return;
        }
        String pass = ssid.substring(sp2 + 1);
        ssid = ssid.substring(0, sp2);
        pass.trim();
        if (ssid.isEmpty() || pass.isEmpty()) {
            Serial.println("[cfg] SSID and password are required");
            return;
        }
        g_config.ssid = ssid;
        g_config.pass = pass;
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        rebootSoon("[cfg] wifi saved, rebooting");
        return;
    }

    if (lower.startsWith("ha ")) {
        int sp = line.indexOf(' ');
        String rest = line.substring(sp + 1);
        rest.trim();
        int sp2 = rest.indexOf(' ');
        String url = (sp2 < 0) ? rest : rest.substring(0, sp2);
        String token = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
        url.trim();
        token.trim();
        if (url.isEmpty()) {
            Serial.println("[cfg] usage: :ha <url> [token]");
            return;
        }
        if (!url.startsWith("http://") && !url.startsWith("https://")) {
            url = "http://" + url;
        }
        while (url.endsWith("/")) url.remove(url.length() - 1);
        g_config.haUrl = url;
        if (!token.isEmpty()) g_config.haToken = token;
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        rebootSoon("[cfg] ha saved, rebooting");
        return;
    }

    if (lower.startsWith("entity ")) {
        int sp = line.indexOf(' ');
        String rest = line.substring(sp + 1);
        rest.trim();
        int sp2 = rest.indexOf(' ');
        if (sp2 < 0) {
            Serial.printf("[cfg] usage: :entity <1-%d> <entity_id>\n", MAX_ENTITIES);
            return;
        }
        int slot = rest.substring(0, sp2).toInt();
        String id = rest.substring(sp2 + 1);
        id.trim();
        id.toLowerCase();
        if (slot < 1 || slot > MAX_ENTITIES) {
            Serial.printf("[cfg] slot must be 1-%d\n", MAX_ENTITIES);
            return;
        }
        g_config.entities[slot - 1] = id;
        int page = (slot - 1) / GRID_SLOTS + 1;
        if (!id.isEmpty() && page > g_config.pageCount) {
            g_config.pageCount = (uint8_t)page;
        }
        g_config.entityCount = (uint8_t)(g_config.pageCount * GRID_SLOTS);
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        rebootSoon("[cfg] entity saved, rebooting");
        return;
    }

    if (lower.startsWith("name ")) {
        int sp = line.indexOf(' ');
        String rest = line.substring(sp + 1);
        rest.trim();
        int sp2 = rest.indexOf(' ');
        if (sp2 < 0) {
            Serial.printf("[cfg] usage: :name <1-%d> <display name>\n", MAX_ENTITIES);
            return;
        }
        int slot = rest.substring(0, sp2).toInt();
        String name = rest.substring(sp2 + 1);
        name.trim();
        if (slot < 1 || slot > MAX_ENTITIES) {
            Serial.printf("[cfg] slot must be 1-%d\n", MAX_ENTITIES);
            return;
        }
        g_config.entityNames[slot - 1] = name;
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        rebootSoon("[cfg] display name saved, rebooting");
        return;
    }

    if (lower.startsWith("bright ")) {
        int value = line.substring(7).toInt();
        if (value < 1 || value > 100) {
            Serial.println("[cfg] usage: :bright <1-100>");
            return;
        }
        g_config.brightness = (uint8_t)value;
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        M5.Display.setBrightness((uint8_t)((value * 255 + 50) / 100));
        Serial.printf("[cfg] brightness=%d%%\n", value);
        return;
    }

    if (lower.startsWith("title ")) {
        int sp = line.indexOf(' ');
        String rest = line.substring(sp + 1);
        rest.trim();
        int sp2 = rest.indexOf(' ');
        if (sp2 < 0) {
            Serial.printf("[cfg] usage: :title <1-%d> <name>\n", MAX_PAGES);
            return;
        }
        int page = rest.substring(0, sp2).toInt();
        String title = rest.substring(sp2 + 1);
        title.trim();
        if (page < 1 || page > MAX_PAGES) {
            Serial.printf("[cfg] page must be 1-%d\n", MAX_PAGES);
            return;
        }
        if (title.isEmpty()) title = "PAGE" + String(page);
        g_config.pageTitles[page - 1] = title;
        if (page > g_config.pageCount) {
            g_config.pageCount = (uint8_t)page;
            g_config.entityCount = (uint8_t)(g_config.pageCount * GRID_SLOTS);
        }
        if (!configSave()) {
            Serial.println("[cfg] NVS write failed");
            return;
        }
        rebootSoon("[cfg] title saved, rebooting");
        return;
    }

    if (lower == "show") {
        printConfig();
        return;
    }

    if (lower == "clear") {
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        p.clear();
        p.end();
        rebootSoon("[cfg] NVS cleared, rebooting");
        return;
    }

    if (lower == "reboot") {
        rebootSoon("[cfg] rebooting");
        return;
    }

    if (lower == "help") {
        Serial.println("[cfg] commands (prefix with ':'):");
        Serial.println("[cfg]   :wifi <ssid> <password>");
        Serial.println("[cfg]   :ha <url> [token]");
        Serial.printf("[cfg]   :entity <1-%d> <entity_id>\n", MAX_ENTITIES);
        Serial.printf("[cfg]   :name <1-%d> <display name>\n", MAX_ENTITIES);
        Serial.printf("[cfg]   :title <1-%d> <name>\n", MAX_PAGES);
        Serial.println("[cfg]   :bright <1-100>");
        Serial.println("[cfg]   :show");
        Serial.println("[cfg]   :clear");
        Serial.println("[cfg]   :reboot");
        return;
    }

    Serial.println("[cfg] unknown command, send :help");
}
