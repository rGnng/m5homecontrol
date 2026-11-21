#include "app_config.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>

#include "ha_client.h"
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
    h += "fieldset{border:1px solid #333;margin:0 0 18px;padding:12px 14px;background:#080808}";
    h += "legend{font-weight:600;padding:0 4px;color:#fff}label{display:block;margin:10px 0 4px;font-size:14px;color:#bbb}";
    h += "input{width:100%;box-sizing:border-box;padding:8px;border:1px solid #444;border-radius:0;font:inherit;background:#111;color:#eee}";
    h += "input:focus{outline:none;border-color:#aaa}";
    h += "button{margin-top:16px;padding:10px 18px;border:1px solid #666;border-radius:0;background:#111;color:#eee;font:inherit}";
    h += "button[type=submit]{background:#eee;color:#000;border-color:#eee}";
    h += "button.remove{margin:0;padding:6px 9px;background:#111;color:#ddd;border:1px solid #555}";
    h += "button.secondary{background:#111;color:#eee;border:1px solid #666;margin:0}";
    h += ".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:20}";
    h += ".modal.open{display:block}";
    h += ".modal-body{background:#0a0a0a;color:#ddd;max-width:640px;max-height:80vh;overflow:auto;margin:10vh auto;padding:18px;border:1px solid #444}";
    h += "table{width:100%;border-collapse:collapse}td{padding:4px 0;vertical-align:top}";
    h += "td.n{width:28px;color:#777;padding-top:12px}code{background:#1a1a1a;color:#eee;padding:1px 4px}";
    h += "th{text-align:left;font-size:12px;color:#888;font-weight:600;padding:4px 0}";
    h += ".modesrow td{padding:0 0 10px}.modelabel{font-size:12px;color:#888;margin-right:8px}";
    h += ".auxfields{display:grid;grid-template-columns:1fr 1fr;gap:8px}";
    h += ".schedule-scroll{overflow-x:auto}.schedule-scroll table{min-width:560px}";
    h += "label.inline{display:inline-flex;align-items:center;gap:4px;margin:0 10px 0 0;font-size:12px;color:#aaa}";
    h += "label.inline input{width:auto;margin:0}";
    h += ".pagehead{font-weight:600;margin:18px 0 6px;color:#fff}";
    h += ".status{border-left:3px solid #eee;padding:8px 10px;background:#111;margin:0 0 14px;color:#bbb}";
    h += ".actions{display:flex;align-items:center;gap:12px;margin:8px 0 24px}";
    h += ".actions button{width:auto;padding:7px 14px;margin:0}";
    h += ".actions.top{justify-content:flex-end;margin:0 0 16px}";
    h += ".actions.bottom{justify-content:flex-end}";
    h += ".save-status{min-height:18px;color:#bbb;font-size:14px;flex:1;margin:0}";
    h += "@media(max-width:800px){.layout{grid-template-columns:1fr}}";
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

static String voltageText(int millivolts) {
    if (millivolts < 0) return "Unavailable";
    return String(millivolts / 1000.0f, 3) + " V";
}

static String powerSummary() {
    PowerStatus power = readPowerStatus();
    String text = "Battery: ";
    text += power.batteryPercent < 0 ? String("Unavailable")
                                    : String(power.batteryPercent) + "% (approximate)";
    text += "\nBattery voltage: " + voltageText(power.batteryMillivolts);
    text += "\nUSB input voltage: " + voltageText(power.inputMillivolts);
    text += "\nCore USB charger: ";
    switch (power.coreCharging) {
        case ChargeState::Charging: text += "Charging"; break;
        case ChargeState::Full: text += "Full"; break;
        case ChargeState::Inactive: text += "Inactive"; break;
        default: text += "Unavailable"; break;
    }
    return text;
}

static void handleRoot() {
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

    String page = htmlHeader("HA Remote Setup");
    page.reserve(40000 + cachedOptions.length());

    String wifiLabel = g_config.ssid.isEmpty()
                           ? String("not configured")
                           : (WiFi.status() == WL_CONNECTED
                                  ? htmlEscape(WiFi.SSID())
                                  : String("disconnected"));
    String haLabel;
    String haColor;
    if (g_config.haToken.isEmpty()) {
        haLabel = "not configured";
        haColor = "#777";
    } else if (haWsConnected()) {
        haLabel = "connected";
        haColor = "#087f23";
    } else {
        haLabel = "disconnected";
        haColor = "#b00020";
    }

    page += "<h1>HA Remote</h1>";
    page += "<form method=\"post\" action=\"/save\">";
    page += "<div class=\"actions top\"><div id=\"save-status\" class=\"save-status\"></div>"
            "<button type=\"submit\">Save and reboot</button></div>";
    page += "<div class=\"layout\">";
    page += "<div class=\"col left\">";

    page += "<fieldset><legend>Config</legend>";
    page += "<div class=\"status\">";
    page += g_apMode ? "Mode: access point" : "Mode: station";
    page += "<br>Address: http://" + htmlEscape(networkIp());
    page += "<br>Wi-Fi: " + wifiLabel;
    page += "<br>Host: " + htmlEscape(g_config.hostname) + ".local";
    page += "<br>Home Assistant: <b style=\"color:" + haColor + "\">" + haLabel + "</b>";
    page += "<br>HA URL: " + htmlEscape(g_config.haUrl);
    page += "</div>";
    page += "<button type=\"button\" class=\"secondary\" onclick=\"openConnection()\">Config: Wi-Fi &amp; Home Assistant</button>";
    page += "</fieldset>";

    page += "<fieldset><legend>Display</legend>";
    page += "<p id=\"power-status\" role=\"status\" style=\"white-space:pre-line\">" +
            htmlEscape(powerSummary()) + "</p>";
    page += "<button type=\"button\" class=\"secondary\" onclick=\"refreshPower()\">Refresh battery status</button>";
    page += "<label>Theme</label><select name=\"theme\">";
    page += "<option value=\"flipper\"" +
            String(g_config.theme == 0 ? " selected" : "") + ">Flipper</option>";
    page += "<option value=\"classic\"" +
            String(g_config.theme == 1 ? " selected" : "") + ">Classic</option>";
    page += "</select>";
    page += "<label>Fallback brightness (%)</label><input name=\"bright\" type=\"number\" min=\"1\" max=\"100\" value=\"" +
            String(g_config.brightness) + "\">";
    page += "<div class=\"pagehead\">Backlight Schedule</div>";
    page += "<div class=\"schedule-scroll\"><table><tbody>";
    page += "<tr><th></th><th>Start</th><th>End</th><th>Brightness %</th>"
            "<th>Dim %</th><th>Dim after (s)</th><th>Off after dim (s)</th></tr>";
    auto scheduleRow = [&](const char* label, const char* prefix, uint16_t start, uint16_t end,
                           uint8_t brightness, uint8_t dimBrightness, uint16_t dim, uint16_t off) {
        page += "<tr><td>" + String(label) + "</td>";
        page += "<td><input name=\"" + String(prefix) + "start\" type=\"time\" value=\"" + formatTime(start) + "\"></td>";
        page += "<td><input name=\"" + String(prefix) + "end\" type=\"time\" value=\"" + formatTime(end) + "\"></td>";
        auto number = [&](const char* suffix, unsigned value, unsigned min, unsigned max) {
            page += "<td><input name=\"" + String(prefix) + suffix + "\" type=\"number\" min=\"" +
                    String(min) + "\" max=\"" + String(max) + "\" value=\"" + String(value) + "\"></td>";
        };
        number("bright", brightness, 0, 100);
        number("dimbright", dimBrightness, 1, 100);
        number("dim", dim, 0, 3600);
        number("off", off, 0, 3600);
        page += "</tr>";
    };
    scheduleRow("day", "d", g_config.dayStart, g_config.dayEnd, g_config.dayBrightness,
                g_config.dayDimBrightness, g_config.dayDimSeconds, g_config.dayOffSeconds);
    scheduleRow("night", "n", g_config.nightStart, g_config.nightEnd, g_config.nightBrightness,
                g_config.nightDimBrightness, g_config.nightDimSeconds, g_config.nightOffSeconds);
    page += "</tbody></table></div>";
    page += "<p>After the dim timeout, use a black background with faint text at Dim brightness. "
            "The off timeout starts when dimming begins. With dim disabled (0), it starts from the last input. "
            "0 disables each timeout. Any input wakes the screen. "
            "Night defaults: dim after 10 minutes, then off 2 minutes later.</p>";
    page += "<p>Schedule time comes from Home Assistant. Local time: " +
            htmlEscape(haLocalTimeText()) + "; time zone: " + htmlEscape(haTimeZone()) + ".</p>";
    page += "<div class=\"pagehead\">Preview display</div>";
    page += "<div class=\"actions\"><button type=\"button\" onclick=\"previewDisplay('wake')\">Wake</button>"
            "<button type=\"button\" onclick=\"previewDisplay('dim')\">Dim</button>"
            "<button type=\"button\" onclick=\"previewDisplay('off')\">Off</button></div>";
    page += "<p id=\"display-status\" role=\"status\">Preview uses the saved brightness settings. "
            "Any device input or Wake restores the normal view.</p>";
    page += "</fieldset>";

    page += "</div>";
    page += "<div class=\"col right\">";
    page += "<fieldset><legend>Entity Grid</legend>";

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
        page += "<div class=\"pagehead\"><span>Page " + String(p + 1) +
                "</span> <button type=\"button\" class=\"remove\" onclick=\"removePage(this)\">Remove page</button></div>";
        page += "<label>Title</label><input class=\"ptitle\" name=\"t" + String(p) +
                "\" value=\"" + htmlEscape(g_config.pageTitles[p]) + "\">";
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
    page += "<button type=\"button\" id=\"add-page\" onclick=\"addPage()\">Add page</button>";
    page += "</fieldset>";
    page += "</div>";
    page += "</div>";

    page += "<div class=\"modal\" id=\"connection-modal\">";
    page += "<div class=\"modal-body\">";
    page += "<div class=\"pagehead\"><span>Config: Wi-Fi &amp; Home Assistant</span> "
            "<button type=\"button\" class=\"remove\" onclick=\"closeConnection()\">Close</button></div>";
    page += "<div class=\"status\">Wi-Fi: " + wifiLabel +
            "<br>Home Assistant: <b style=\"color:" + haColor + "\">" + haLabel +
            "</b><br>HA URL: " + htmlEscape(g_config.haUrl) + "</div>";
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
    page += "<button type=\"submit\">Save and reboot</button>";
    page += "</div></div>";

    page += String(R"JS(
<script>
const MAX_PAGES = )JS") + String(MAX_PAGES) + R"JS(;
const SLOTS = )JS" + String(GRID_SLOTS) + R"JS(;
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
    block.querySelector('.pagehead span').textContent = 'Page ' + (p + 1);
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
  block.innerHTML = '<div class="pagehead"><span></span> <button type="button" class="remove" onclick="removePage(this)">Remove page</button></div>' +
    '<label>Title</label><input class="ptitle" value="PAGE' + (blocks.length + 1) + '">' +
    '<table><tbody><tr><th></th><th>Entity ID</th><th>Display name</th></tr>' + rows + '</tbody></table>';
  document.getElementById('pages').appendChild(block);
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
    const response = await fetch('/power', {cache: 'no-store'});
    if (!response.ok) throw new Error('Power status unavailable');
    status.textContent = await response.text();
  } catch (error) {
    status.textContent = 'Could not read battery status. Check the connection and retry.';
  }
}
async function previewDisplay(mode) {
  const status = document.getElementById('display-status');
  try {
    const response = await fetch('/display', {
      method: 'POST', body: new URLSearchParams({mode})
    });
    if (!response.ok) throw new Error('Display control failed');
    status.textContent = mode === 'wake' ? 'Display awake.' :
      (mode === 'dim' ? 'Dim preview active.' : 'Display off. Use Wake to turn it on.');
  } catch (error) {
    status.textContent = 'Could not reach the display. Try again.';
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

    page += "<div class=\"actions bottom\"><button type=\"submit\">Save and reboot</button></div>";
    page += "</form>";
    page += "<p><a href=\"/update\">Update firmware</a> &middot; "
            "<a href=\"/screenshot.bmp\" download=\"ha-remote-screen.bmp\">Download screenshot</a></p>";
    page += "<p style=\"margin-top:24px;color:#777\">Serial console: send <code>:help</code> at 115200 baud.</p>";
    page += htmlFooter();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", page);
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
<button id="upload" type="submit">Upload and restart</button>
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

static void handleFirmwareUpload() {
    // Upload callbacks run before the final POST handler. Authenticate here
    // as well so rejected requests never write to flash.
    if (!firmwareAuthenticated()) return;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
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
    }
}

static void handleFirmwareComplete() {
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
    // Read the panel's pixels without redrawing, navigating, or waking it.
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
            server.send(200, "text/plain", powerSummary());
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
            text += " " + menuStatus();
            text += " " + backlightStatus();
            text += " " + haClockStatus();
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
    WiFi.setSleep(false);
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
