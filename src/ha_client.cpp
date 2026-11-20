#include "ha_client.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <esp_timer.h>

#include "app_config.h"
#include "ha_clock.h"

static WebSocketsClient ws;
static bool g_wsStarted = false;
static bool g_wsAuthed = false;
static uint32_t g_wsMessageId = 1;
static uint32_t g_wsClockId = 0;
static uint32_t g_clockRetryAt = 0;
static HaClock g_haClock;
static String g_haTimeZone;

static uint64_t clockMillis() { return esp_timer_get_time() / 1000; }

bool haGetLocalTime(struct tm& timeinfo) {
    return g_haClock.localTime(clockMillis(), timeinfo);
}

String haTimeZone() { return g_haTimeZone.isEmpty() ? String("Unavailable") : g_haTimeZone; }

String haLocalTimeText() {
    struct tm value;
    if (!haGetLocalTime(value)) return String("Unavailable");
    char text[24];
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &value);
    return String(text);
}

String haClockStatus() {
    String text = "time_source=" + String(g_haClock.ready() ? "ha" : "unavailable");
    text += " ha_timezone=" + haTimeZone();
    text += " utc_offset=" + String(g_haClock.offsetSeconds());
    text += " local_time=" + haLocalTimeText();
    text += " ha_clock_age=" + String(g_haClock.ready()
            ? static_cast<long>(g_haClock.ageMs(clockMillis()) / 1000) : -1L);
    return text;
}

static HaStateCallback g_stateCb = nullptr;
static HaStatusCallback g_statusCb = nullptr;
static HaAuthCallback g_authCb = nullptr;
static HaCommandCallback g_commandCb = nullptr;

// ---------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------
String asciiText(const String& text) {
    String out;
    out.reserve(text.length());
    for (size_t i = 0; i < text.length(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 32 && c < 127) out += (char)c;
    }
    out.trim();
    return out;
}

String entityDomain(const String& id) {
    int dot = id.indexOf('.');
    return dot > 0 ? id.substring(0, dot) : String("");
}

EntityType entityTypeFromId(const String& id) {
    if (id.startsWith("light.")) return EntityType::Light;
    if (id.startsWith("switch.")) return EntityType::Switch;
    if (id.startsWith("input_boolean.")) return EntityType::Switch;
    if (id.startsWith("fan.")) return EntityType::Switch;
    if (id.startsWith("climate.")) return EntityType::Climate;
    if (id.startsWith("scene.")) return EntityType::Scene;
    if (id.startsWith("script.")) return EntityType::Scene;
    if (id.startsWith("media_player.")) return EntityType::Media;
    if (id.startsWith("sensor.")) return EntityType::Sensor;
    if (id.startsWith("binary_sensor.")) return EntityType::Sensor;
    if (id.startsWith("cover.")) return EntityType::Cover;
    return EntityType::Unknown;
}

String defaultNameFromId(const String& id) {
    int dot = id.indexOf('.');
    String raw = dot >= 0 ? id.substring(dot + 1) : id;
    String out;
    out.reserve(raw.length());
    bool cap = true;
    for (size_t i = 0; i < raw.length(); i++) {
        char c = raw[i];
        if (c == '_' || c == '-') {
            out += ' ';
            cap = true;
        } else if (cap) {
            out += (char)toupper((unsigned char)c);
            cap = false;
        } else {
            out += c;
        }
    }
    return out;
}

static void notifyStatus(bool online) {
    if (g_statusCb) g_statusCb(online);
}

static void deriveOn(EntityState& e) {
    switch (e.type) {
        case EntityType::Light:
        case EntityType::Switch:
            e.on = (e.state == "on");
            break;
        case EntityType::Climate:
            e.on = (e.state != "off" && e.state != "unavailable");
            break;
        case EntityType::Media:
            e.on = (e.state == "playing" || e.state == "on");
            break;
        case EntityType::Cover:
            e.on = (e.state == "open" || e.state == "opening");
            break;
        default:
            e.on = false;
            break;
    }
}

static String joinArray(JsonVariantConst value) {
    if (!value.is<JsonArrayConst>()) return String("");
    String out;
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
        if (!out.isEmpty()) out += ",";
        out += item.as<String>();
    }
    return out;
}

static bool parseState(EntityState& e, JsonVariantConst obj) {
    if (obj.isNull()) {
        e.state = "unavailable";
        e.available = false;
        return true;
    }

    e.type = entityTypeFromId(e.id);
    e.state = obj["state"].as<String>();
    JsonVariantConst attrs = obj["attributes"];

    e.name = attrs["friendly_name"].as<String>();
    if (e.name.isEmpty()) e.name = defaultNameFromId(e.id);
    e.name = asciiText(e.name);

    e.unit = asciiText(attrs["unit_of_measurement"].as<String>());
    e.brightness = attrs["brightness"] | -1;
    e.brightnessAdjustable = false;
    if (e.type == EntityType::Light) {
        JsonArrayConst modes = attrs["supported_color_modes"].as<JsonArrayConst>();
        if (!modes.isNull() && modes.size()) {
            for (JsonVariantConst item : modes) {
                String mode = item.as<String>();
                if (mode != "onoff" && mode != "unknown" && !mode.isEmpty()) {
                    e.brightnessAdjustable = true;
                }
            }
        } else {
            // Older integrations may omit color modes and advertise the old
            // SUPPORT_BRIGHTNESS bit, or expose a current brightness instead.
            e.brightnessAdjustable = e.brightness >= 0 ||
                                     ((attrs["supported_features"] | 0U) & 1U);
        }
    }
    e.current = attrs["current_temperature"] | NAN;
    e.target = attrs["temperature"] | NAN;
    e.temperatureAdjustable = e.type == EntityType::Climate &&
        (((attrs["supported_features"] | 0U) & 1U) ||
         (attrs["supported_features"].isNull() && !attrs["temperature"].isNull()));

    e.hvacAction = attrs["hvac_action"].as<String>();
    e.hvacModes = joinArray(attrs["hvac_modes"]);
    e.fanModes = joinArray(attrs["fan_modes"]);
    e.swingModes = joinArray(attrs["swing_modes"]);
    e.fanMode = attrs["fan_mode"].as<String>();
    e.swingMode = attrs["swing_mode"].as<String>();
    e.presetModes = joinArray(attrs["preset_modes"]);
    e.presetMode = attrs["preset_mode"].as<String>();
    e.minTemp = attrs["min_temp"] | 16.0f;
    e.maxTemp = attrs["max_temp"] | 30.0f;
    e.tempStep = attrs["target_temp_step"] | 0.5f;
    if (e.tempStep <= 0.0f) e.tempStep = 0.5f;

    e.detail = attrs["media_title"].as<String>();
    if (e.detail.isEmpty()) e.detail = attrs["device_class"].as<String>();
    e.detail = asciiText(e.detail);

    e.available = (e.state != "unavailable");
    deriveOn(e);
    return true;
}

static String escapeHtml(const String& in) {
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

// ---------------------------------------------------------------------
//  HTTP (initial state fetch and service calls)
// ---------------------------------------------------------------------
static bool httpRequest(const String& method, const String& path,
                        const String& body, String& response) {
    if (!configComplete()) return false;

    String url = g_config.haUrl + path;
    bool tls = url.startsWith("https://");

    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;

    if (tls) {
        secure.setInsecure();
        http.begin(secure, url);
    } else {
        http.begin(plain, url);
    }
    http.setTimeout(tls ? 4000 : 1200);
    http.setConnectTimeout(tls ? 4000 : 1200);
    http.setReuse(false);
    http.addHeader("Authorization", "Bearer " + g_config.haToken);

    int code;
    if (method == "GET") {
        code = http.GET();
    } else {
        http.addHeader("Content-Type", "application/json");
        if (g_commandCb) g_commandCb();
        code = http.POST(body);
    }

    bool ok = (code >= 200 && code < 300);
    response = "";
    if (ok) {
        response = http.getString();
    } else if (code < 0) {
        Serial.printf("[ha] http %s %s -> %d\n", method.c_str(), path.c_str(), code);
    } else {
        Serial.printf("[ha] http %s %s -> %d\n", method.c_str(), path.c_str(), code);
    }
    http.end();
    return ok;
}

bool haFetchState(EntityState& entity) {
    if (!configComplete() || entity.id.isEmpty()) return false;

    String body;
    if (!httpRequest("GET", "/api/states/" + entity.id, "", body)) {
        entity.available = false;
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        entity.available = false;
        return false;
    }

    parseState(entity, doc.as<JsonVariantConst>());
    return true;
}

bool haFetchEntityOptions(String& optionsHtml, int maxOptions) {
    if (!configComplete()) return false;

    String url = g_config.haUrl + "/api/states";
    bool tls = url.startsWith("https://");

    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    if (tls) {
        secure.setInsecure();
        http.begin(secure, url);
    } else {
        http.begin(plain, url);
    }
    http.setTimeout(tls ? 8000 : 5000);
    http.setConnectTimeout(tls ? 6000 : 3000);
    http.setReuse(false);
    http.addHeader("Authorization", "Bearer " + g_config.haToken);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[ha] entity list -> %d\n", code);
        http.end();
        return false;
    }

    JsonDocument filter;
    filter[0]["entity_id"] = true;
    filter[0]["attributes"]["friendly_name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Serial.printf("[ha] entity list json %s\n", err.c_str());
        return false;
    }

    JsonArray array = doc.as<JsonArray>();
    optionsHtml.reserve(3000);
    int added = 0;
    for (JsonObject object : array) {
        String id = object["entity_id"].as<String>();
        if (id.isEmpty()) continue;
        String name = object["attributes"]["friendly_name"].as<String>();
        if (name.isEmpty()) name = id;
        optionsHtml += "<option value=\"";
        optionsHtml += escapeHtml(id);
        optionsHtml += "\">";
        optionsHtml += escapeHtml(name);
        optionsHtml += "</option>";
        if (++added >= maxOptions) break;
    }
    Serial.printf("[ha] entity list loaded=%d\n", added);
    return added > 0;
}

bool haCallService(const String& domain, const String& service,
                   const String& entityId, const String& extraJson) {
    if (!configComplete() || domain.isEmpty() || service.isEmpty() || entityId.isEmpty()) {
        return false;
    }

    String body = "{\"entity_id\":\"" + entityId + "\"";
    if (!extraJson.isEmpty()) body += "," + extraJson;
    body += "}";

    String response;
    bool ok = httpRequest("POST", "/api/services/" + domain + "/" + service, body, response);
    if (!ok) Serial.printf("[ha] service %s.%s failed\n", domain.c_str(), service.c_str());
    return ok;
}

// ---------------------------------------------------------------------
//  WebSocket
// ---------------------------------------------------------------------
static void wsSendJson(JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
}

static void wsSubscribeClock() {
    JsonDocument doc;
    g_wsClockId = g_wsMessageId++;
    doc["id"] = g_wsClockId;
    doc["type"] = "render_template";
    doc["template"] = "{% set t = now() %}{{ {'epoch': as_timestamp(t) | int, "
                      "'utc_offset': t.utcoffset().total_seconds() | int, "
                      "'time_zone': t.tzinfo | string} | to_json }}";
    doc["report_errors"] = true;
    wsSendJson(doc);
    g_clockRetryAt = millis() + 180000;
}

static bool syncClock(JsonVariantConst result) {
    JsonDocument parsed;
    if (result.is<const char*>()) {
        if (deserializeJson(parsed, result.as<const char*>())) return false;
        result = parsed.as<JsonVariantConst>();
    }
    if (!result["epoch"].is<int64_t>() || !result["utc_offset"].is<int32_t>() ||
        !result["time_zone"].is<const char*>()) return false;
    String zone = result["time_zone"].as<String>();
    if (zone.isEmpty() || zone.length() > 96) return false;
    if (!g_haClock.sync(result["epoch"].as<int64_t>(),
                        result["utc_offset"].as<int32_t>(), clockMillis())) return false;
    bool changed = zone != g_haTimeZone;
    g_haTimeZone = zone;
    g_clockRetryAt = millis() + 180000;
    if (changed) Serial.printf("[ha] clock zone=%s offset=%ld\n", zone.c_str(),
                               static_cast<long>(g_haClock.offsetSeconds()));
    return true;
}

static void wsSubscribe() {
    JsonDocument doc;
    doc["id"] = g_wsMessageId++;
    doc["type"] = "subscribe_events";
    doc["event_type"] = "state_changed";
    wsSendJson(doc);
    Serial.println("[ha] subscribed to state_changed");
}

static void wsHandleText(const uint8_t* payload, size_t length) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (const char*)payload, length);
    if (err) return;

    String type = doc["type"].as<String>();
    if (type == "auth_required") {
        JsonDocument auth;
        auth["type"] = "auth";
        auth["access_token"] = g_config.haToken;
        wsSendJson(auth);
        return;
    }

    if (type == "auth_ok") {
        g_wsAuthed = true;
        notifyStatus(true);
        Serial.println("[ha] ws authenticated");
        wsSubscribe();
        wsSubscribeClock();
        if (g_authCb) g_authCb();
        return;
    }

    if (type == "auth_invalid") {
        g_wsAuthed = false;
        notifyStatus(false);
        Serial.println("[ha] auth invalid, will retry");
        ws.disconnect();
        return;
    }

    uint32_t id = doc["id"] | 0U;
    if (g_wsClockId && id == g_wsClockId) {
        if (type == "event") {
            if (!syncClock(doc["event"]["result"])) {
                Serial.println("[ha] clock update invalid; will retry");
                g_clockRetryAt = millis() + 30000;
            }
        } else if (type == "result" && !(doc["success"] | false)) {
            g_wsClockId = 0;
            g_clockRetryAt = millis() + 30000;
            Serial.println("[ha] clock subscription failed; will retry");
        }
        return;
    }

    if (type == "event") {
        String eventType = doc["event"]["event_type"].as<String>();
        if (eventType != "state_changed") return;

        String entityId = doc["event"]["data"]["entity_id"].as<String>();
        JsonVariantConst newState = doc["event"]["data"]["new_state"];
        if (entityId.isEmpty()) return;

        EntityState state;
        state.id = entityId;
        parseState(state, newState);
        if (g_stateCb) g_stateCb(state);
    }
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            g_wsAuthed = false;
            g_wsClockId = 0;
            Serial.println("[ha] ws connected");
            break;
        case WStype_DISCONNECTED:
            g_wsAuthed = false;
            g_wsClockId = 0;
            notifyStatus(false);
            Serial.println("[ha] ws disconnected");
            break;
        case WStype_TEXT:
            wsHandleText(payload, length);
            break;
        case WStype_ERROR:
            Serial.println("[ha] ws error");
            break;
        default:
            break;
    }
}

void haBegin() {
    if (!configComplete()) {
        Serial.println("[ha] ws not started: missing url or token");
        return;
    }
    if (g_wsStarted) return;

    String url = g_config.haUrl;
    bool ssl = url.startsWith("https://");
    int scheme = url.indexOf("://");
    String rest = (scheme >= 0) ? url.substring(scheme + 3) : url;
    int slash = rest.indexOf('/');
    String hostPort = (slash >= 0) ? rest.substring(0, slash) : rest;
    int colon = hostPort.indexOf(':');
    String host = (colon >= 0) ? hostPort.substring(0, colon) : hostPort;
    int port = (colon >= 0) ? hostPort.substring(colon + 1).toInt() : (ssl ? 443 : 80);

    if (host.isEmpty()) {
        Serial.println("[ha] invalid host");
        return;
    }

    ws.onEvent(wsEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);

    if (ssl) {
        ws.beginSSL(host.c_str(), port, "/api/websocket");
    } else {
        ws.begin(host.c_str(), port, "/api/websocket");
    }
    g_wsStarted = true;
    Serial.printf("[ha] ws connecting to %s:%d\n", host.c_str(), port);
}

void haLoop() {
    if (g_wsStarted) ws.loop();
    if (g_wsAuthed && (int32_t)(millis() - g_clockRetryAt) >= 0) {
        if (g_wsClockId) {
            JsonDocument doc;
            doc["id"] = g_wsMessageId++;
            doc["type"] = "unsubscribe_events";
            doc["subscription"] = g_wsClockId;
            wsSendJson(doc);
        }
        wsSubscribeClock();
    }
}

bool haWsConnected() {
    return g_wsStarted && g_wsAuthed;
}

void haSetStateCallback(HaStateCallback cb) {
    g_stateCb = cb;
}

void haSetStatusCallback(HaStatusCallback cb) {
    g_statusCb = cb;
}

void haSetAuthCallback(HaAuthCallback cb) {
    g_authCb = cb;
}

void haSetCommandCallback(HaCommandCallback cb) {
    g_commandCb = cb;
}
