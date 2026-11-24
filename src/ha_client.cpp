#include "ha_client.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <esp_timer.h>
#include <atomic>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "app_config.h"
#include "connected_power.h"
#include "deadline_stream.h"
#include "transfer_budget.h"
#include "network_io.h"
#include "async_websocket.h"
#include "ha_clock.h"
#include "utf8_text.h"

static AsyncWebSocket ws;
static bool g_wsStarted = false;
static bool g_dataSuspended = false, g_wsResumePending = false;
static uint32_t g_dataGeneration = 0, g_discardedHttp = 0;

bool haDataSuspended() { return g_dataSuspended; }
uint32_t haDataGeneration() { return g_dataGeneration; }
static bool g_wsAuthed = false;
static uint32_t g_wsMessageId = 1;
static uint32_t g_wsClockId = 0;
static uint32_t g_clockRetryAt = 0;
static uint32_t g_wsStateId = 0, g_stateRetryAt = 0;
static bool g_stateSubscribed = false;
static unsigned g_watchedEntities = 0;
static HaClock g_haClock;
static String g_haTimeZone;

struct HaLinkStats {
    uint32_t connections = 0, disconnects = 0, authentications = 0, errors = 0;
    uint32_t lastConnectMs = 0, lastDisconnectMs = 0, lastAuthMs = 0;
    uint32_t maxLoopGapMs = 0;
    uint64_t lastLoopMs = 0;
    String lastDisconnectTime;
    int lastCommandStatus = 0;
    uint32_t commandFailures = 0, lastCommandMs = 0;
    uint32_t stateSequence = 0;
    uint32_t stateEvents = 0, ignoredStateEvents = 0, subscriptionErrors = 0;
};
static HaLinkStats g_linkStats;

static uint64_t clockMillis() { return esp_timer_get_time() / 1000; }

bool haClockReady() { return g_haClock.ready(); }

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

String haConnectionStatus() {
    String text = "wifi_connected=" + String(networkConnected() ? 1 : 0);
    text += " ha_data_suspended=" + String(g_dataSuspended ? 1 : 0);
    text += " ha_data_generation=" + String(g_dataGeneration);
    text += " ha_http_discarded=" + String(g_discardedHttp);
    text += " ws_connected=" + String(haWsConnected() ? 1 : 0);
    text += " ws_connections=" + String(g_linkStats.connections);
    text += " ws_disconnects=" + String(g_linkStats.disconnects);
    text += " ws_authentications=" + String(g_linkStats.authentications);
    text += " ws_reconnects=" + String(g_linkStats.authentications > 0
            ? g_linkStats.authentications - 1 : 0);
    text += " ws_errors=" + String(g_linkStats.errors);
    text += " ws_last_connect_ms=" + String(g_linkStats.lastConnectMs);
    text += " ws_last_disconnect_ms=" + String(g_linkStats.lastDisconnectMs);
    text += " ws_last_auth_ms=" + String(g_linkStats.lastAuthMs);
    text += " ws_last_disconnect_time=" + (g_linkStats.lastDisconnectTime.isEmpty()
            ? String("unavailable") : g_linkStats.lastDisconnectTime);
    text += " ws_loop_max_gap_ms=" + String(g_linkStats.maxLoopGapMs);
    text += " ha_command_failures=" + String(g_linkStats.commandFailures);
    text += " ha_last_command_status=" + String(g_linkStats.lastCommandStatus);
    text += " ha_last_command_ms=" + String(g_linkStats.lastCommandMs);
    text += " ws_state_ready=" + String(haStateUpdatesReady() ? 1 : 0);
    text += " ws_watched_entities=" + String(g_watchedEntities);
    text += " ws_state_events=" + String(g_linkStats.stateEvents);
    text += " ws_ignored_events=" + String(g_linkStats.ignoredStateEvents);
    text += " ws_subscription_errors=" + String(g_linkStats.subscriptionErrors);
    return text;
}

static HaStateCallback g_stateCb = nullptr;
static HaStatusCallback g_statusCb = nullptr;
static HaAuthCallback g_authCb = nullptr;
static HaCommandCallback g_commandCb = nullptr;

// ---------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------
String displayText(const String& text) {
    String out = ui_text::clean(text);
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
    e.name = displayText(e.name);

    String unit = attrs["unit_of_measurement"].as<String>();
    // Preserve the requested compact temperature units.
    if (unit == "℃") unit = "C";
    else if (unit == "℉") unit = "F";
    e.unit = displayText(unit);
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
    e.detail = displayText(e.detail);

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
// The worker owns only request snapshots and result buffers. All entity/cache
// changes, power guards, LEDs and completions belong to the main loop.
struct HttpWork {
    std::function<bool(HttpWork&)> operation;
    std::function<void(bool, const String&, int)> complete;
    std::atomic<bool> done{false}, cancelled{false};
    bool used = false, command = false, ok = false, powerHeld = false;
    String entityId, response;
    int status = 0;
    uint64_t order = 0;
    uint32_t started = 0, generation = 0;
};
static constexpr unsigned HTTP_WORK_LIMIT = 8;
static HttpWork g_httpWork[HTTP_WORK_LIMIT];
static HttpWork* g_httpActive = nullptr;
static QueueHandle_t g_httpQueue = nullptr;
static uint64_t g_httpOrder = 0;

static void httpWorkerTask(void*) {
    for (;;) {
        HttpWork* work = nullptr;
        if (xQueueReceive(g_httpQueue, &work, portMAX_DELAY) == pdTRUE) {
            work->ok = !work->cancelled.load(std::memory_order_acquire) && work->operation(*work);
            work->done.store(true, std::memory_order_release);
        }
    }
}

bool haHttpBusy() {
    for (const auto& work : g_httpWork) if (work.used) return true;
    return false;
}

static bool runHttpWork(std::function<bool(HttpWork&)> operation,
                        std::function<void(bool, const String&, int)> complete,
                        bool command = false, const String& entityId = "") {
    // Background reads never queue behind commands. State-based commands for
    // one entity cannot overlap and calculate a second value from stale state.
    if (g_dataSuspended || (!command && haHttpBusy())) return false;
    HttpWork* slot = nullptr;
    for (auto& work : g_httpWork) {
        if (command && work.used && work.command && work.entityId == entityId) return false;
        if (!work.used && !slot) slot = &work;
    }
    if (!slot) return false;
    if (!g_httpQueue) {
        g_httpQueue = xQueueCreate(1, sizeof(HttpWork*));
        if (!g_httpQueue) return false;
        if (xTaskCreate(httpWorkerTask, "ha-http", 12288, nullptr, 1, nullptr) != pdPASS) {
            vQueueDelete(g_httpQueue);
            g_httpQueue = nullptr;
            return false;
        }
    }
    slot->operation = std::move(operation);
    slot->complete = std::move(complete);
    slot->command = command;
    slot->entityId = entityId;
    slot->response = "";
    slot->status = 0;
    slot->ok = false;
    slot->powerHeld = false;
    slot->done.store(false, std::memory_order_relaxed);
    slot->cancelled.store(false, std::memory_order_relaxed);
    slot->generation = g_dataGeneration;
    slot->order = ++g_httpOrder;
    slot->used = true;
    return true;
}

static void pollHttpWork() {
    if (g_httpActive && g_httpActive->done.load(std::memory_order_acquire)) {
        HttpWork& work = *g_httpActive;
        bool current = work.generation == g_dataGeneration;
        bool ok = current && work.ok;
        if (!current) ++g_discardedHttp;
        int status = work.status;
        String response = std::move(work.response);
        auto complete = std::move(work.complete);
        if (work.command && current) {
            g_linkStats.lastCommandStatus = status;
            g_linkStats.lastCommandMs = millis() - work.started;
            if (!ok) ++g_linkStats.commandFailures;
            Serial.printf("[ha] command status=%d duration=%lums ws=%d\n", status,
                          static_cast<unsigned long>(g_linkStats.lastCommandMs), haWsConnected());
        }
        work.operation = nullptr;
        work.entityId = "";
        work.used = false;
        g_httpActive = nullptr;
        if (work.powerHeld) {
            work.powerHeld = false;
            endConnectedPowerWork();
        }
        if (complete) complete(ok, response, status);
    }
    if (g_httpActive) return;
    HttpWork* next = nullptr;
    for (auto& work : g_httpWork) {
        if (!work.used) continue;
        if (!next || (work.command && !next->command) ||
            (work.command == next->command && work.order < next->order)) next = &work;
    }
    if (!next) return;
    g_httpActive = next;
    next->started = millis();
    beginConnectedPowerWork();
    next->powerHeld = true;
    if (next->command && g_commandCb) g_commandCb();
    if (xQueueSend(g_httpQueue, &next, 0) != pdTRUE) {
        next->done.store(true, std::memory_order_release);
    }
}

void haSetDataSuspended(bool suspended) {
    if (g_dataSuspended == suspended) return;
    g_dataSuspended = suspended;
    ws.setSuspended(suspended);
    ++g_dataGeneration;
    if (suspended) {
        g_wsResumePending = false;
        g_wsAuthed = g_stateSubscribed = false;
        g_wsClockId = g_wsStateId = 0;
        if (g_httpActive) {
            g_httpActive->cancelled.store(true, std::memory_order_release);
            // A slow cancelled transfer must not hold the CPU/radio awake.
            if (g_httpActive->powerHeld) {
                g_httpActive->powerHeld = false;
                endConnectedPowerWork();
            }
        }
        if (g_wsStarted) ws.disconnect();
        notifyStatus(false);
        // The worker may still own the active request; never free or reuse it.
        // Queued operations have not reached HA and can be cancelled immediately.
        for (auto& work : g_httpWork) {
            if (!work.used || &work == g_httpActive) continue;
            auto complete = std::move(work.complete);
            work.operation = nullptr;
            work.response = work.entityId = "";
            work.used = false;
            ++g_discardedHttp;
            if (complete) complete(false, "", 0);
        }
    } else {
        g_linkStats.lastLoopMs = 0; // Suspension is not a WebSocket scheduling stall.
        g_wsResumePending = g_wsStarted;
        if (g_wsResumePending) ws.setReconnectInterval(0);
    }
}

static bool httpRequestBlocking(const String& baseUrl, const String& token,
                                const String& method, const String& path,
                                const String& body, String& response, int& status,
                                const std::atomic<bool>* cancelled = nullptr) {
    String url = baseUrl + path;
    bool tls = url.startsWith("https://");

    HTTPClient http;
    TransferBudget budget(tls ? 10000 : 5000, 65536, cancelled);
    BudgetClient<ResponsiveClient> plain(budget);
    BudgetClient<ResponsiveSecureClient> secure(budget);
    secure.setHandshakeTimeout(4);

    if (tls) {
        secure.setInsecure();
        http.begin(secure, url);
    } else {
        http.begin(plain, url);
    }
    http.setTimeout(tls ? 4000 : 1200);
    http.setConnectTimeout(tls ? 4000 : 1200);
    http.setReuse(false);
    http.addHeader("Authorization", "Bearer " + token);

    int code;
    if (method == "GET") {
        code = http.GET();
    } else {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(body);
    }
    status = code;

    bool ok = (code >= 200 && code < 300);
    response = "";
    if (ok) {
        if (http.getSize() > 65536) ok = false;
        else response = http.getString();
        if (!budget.allowed()) ok = false;
    } else if (code < 0) {
        Serial.printf("[ha] http %s %s -> %d\n", method.c_str(), path.c_str(), code);
    } else {
        Serial.printf("[ha] http %s %s -> %d\n", method.c_str(), path.c_str(), code);
    }
    http.end();
    return ok;
}

bool haFetchState(EntityState& entity, HaResultCallback complete) {
    if (!configComplete() || entity.id.isEmpty()) return false;
    const String id = entity.id, url = g_config.haUrl, token = g_config.haToken;
    const uint32_t revision = entity.revision, generation = g_dataGeneration;
    return runHttpWork([url, token, id](HttpWork& work) {
        return httpRequestBlocking(url, token, "GET", "/api/states/" + id, "",
                                   work.response, work.status, &work.cancelled);
    }, [&entity, id, revision, generation, complete](bool ok, const String& body, int) {
        if (generation != g_dataGeneration) { if (complete) complete(false); return; }
        // Live updates received after submission always win, even after errors.
        if (entity.id != id) { if (complete) complete(false); return; }
        if (entity.revision != revision) { if (complete) complete(true); return; }
        if (ok) {
            JsonDocument doc;
            ok = !deserializeJson(doc, body);
            if (ok) parseState(entity, doc.as<JsonVariantConst>());
        }
        if (!ok) entity.available = false;
        if (complete) complete(ok);
    });
}

static bool fetchEntityOptionsBlocking(const String& baseUrl, const String& token,
                                       String& optionsHtml, int maxOptions,
                                       const std::atomic<bool>* cancelled = nullptr) {
    String url = baseUrl + "/api/states";
    bool tls = url.startsWith("https://");

    HTTPClient http;
    TransferBudget budget(20000, 4 * 1024 * 1024, cancelled);
    BudgetClient<ResponsiveClient> plain(budget);
    BudgetClient<ResponsiveSecureClient> secure(budget);
    secure.setHandshakeTimeout(4);
    if (tls) {
        secure.setInsecure();
        http.begin(secure, url);
    } else {
        http.begin(plain, url);
    }
    http.setTimeout(tls ? 8000 : 5000);
    http.setConnectTimeout(tls ? 6000 : 3000);
    http.setReuse(false);
    http.addHeader("Authorization", "Bearer " + token);

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
    DeadlineStream<WiFiClient> input(http.getStream(), 15000);
    DeserializationError err = deserializeJson(
        doc, input, DeserializationOption::Filter(filter));
    http.end();

    if (err || !budget.allowed()) {
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

bool haFetchEntityOptions(HaOptionsCallback complete, int maxOptions) {
    if (!configComplete()) return false;
    const String url = g_config.haUrl, token = g_config.haToken;
    return runHttpWork([url, token, maxOptions](HttpWork& work) {
        return fetchEntityOptionsBlocking(url, token, work.response, maxOptions, &work.cancelled);
    }, [complete](bool ok, const String& options, int) {
        if (complete) complete(ok, options);
    });
}

bool haCallService(const String& domain, const String& service,
                   const String& entityId, const String& extraJson,
                   HaResultCallback complete) {
    if (!configComplete() || domain.isEmpty() || service.isEmpty() || entityId.isEmpty()) {
        return false;
    }
    String body = "{\"entity_id\":\"" + entityId + "\"";
    if (!extraJson.isEmpty()) body += "," + extraJson;
    body += "}";
    const String url = g_config.haUrl, token = g_config.haToken;
    const String path = "/api/services/" + domain + "/" + service;
    return runHttpWork([url, token, path, body](HttpWork& work) {
        return httpRequestBlocking(url, token, "POST", path, body, work.response, work.status, &work.cancelled);
    }, [complete](bool ok, const String&, int) {
        if (complete) complete(ok);
    }, true, entityId);
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

static bool stateEntityConfigured(const String& id) {
    if (id.isEmpty()) return false;
    for (int i = 0; i < g_config.entityCount; ++i) {
        if (g_config.entities[i] == id || g_config.auxEntities[i] == id) return true;
    }
    return false;
}

static void wsSubscribe() {
    JsonDocument doc;
    g_stateSubscribed = false;
    g_wsStateId = 0;
    doc["type"] = "subscribe_trigger";
    doc["trigger"]["platform"] = "state";
    JsonArray ids = doc["trigger"]["entity_id"].to<JsonArray>();
    for (int i = 0; i < g_config.entityCount; ++i) {
        for (const String* id : {&g_config.entities[i], &g_config.auxEntities[i]}) {
            if (id->isEmpty()) continue;
            bool duplicate = false;
            for (JsonVariantConst existing : ids) {
                if (existing.as<String>() == *id) { duplicate = true; break; }
            }
            if (!duplicate) ids.add(*id);
        }
    }
    g_watchedEntities = ids.size();
    // An empty filter must never turn into an all-HA subscription.
    if (!g_watchedEntities) { g_stateSubscribed = true; return; }
    g_wsStateId = g_wsMessageId++;
    doc["id"] = g_wsStateId;
    g_stateRetryAt = millis() + 30000;
    wsSendJson(doc);
    Serial.printf("[ha] subscribing to %u configured entities\n", g_watchedEntities);
}

static void wsHandleText(const uint8_t* payload, size_t length) {
    if (g_dataSuspended) return;
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
        ++g_linkStats.authentications;
        g_linkStats.lastAuthMs = millis();
        notifyStatus(true);
        Serial.println("[ha] ws authenticated");
        wsSubscribe();
        wsSubscribeClock();
        if (g_authCb) g_authCb();
        return;
    }

    if (type == "auth_invalid") {
        g_wsAuthed = false;
        g_stateSubscribed = false;
        g_wsStateId = 0;
        ++g_linkStats.errors;
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

    if (g_wsStateId && id == g_wsStateId && type == "result") {
        g_stateSubscribed = doc["success"] | false;
        if (!g_stateSubscribed) {
            ++g_linkStats.subscriptionErrors;
            g_wsStateId = 0;
            g_stateRetryAt = millis() + 30000;
            Serial.println("[ha] state subscription rejected; REST fallback and retry enabled");
        }
        return;
    }

    if (type == "event") {
        JsonVariantConst trigger = doc["event"]["variables"]["trigger"];
        String entityId = trigger["entity_id"].as<String>();
        if (!g_wsAuthed || !g_stateSubscribed || !g_wsStateId || id != g_wsStateId ||
            !stateEntityConfigured(entityId)) {
            ++g_linkStats.ignoredStateEvents;
            return;
        }
        JsonVariantConst newState = trigger["to_state"];
        if (!newState.isNull() && newState["entity_id"].as<String>() != entityId) {
            ++g_linkStats.ignoredStateEvents;
            return;
        }

        EntityState state;
        state.id = entityId;
        parseState(state, newState);
        state.revision = ++g_linkStats.stateSequence;
        ++g_linkStats.stateEvents;
        if (g_stateCb) g_stateCb(state);
    }
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            g_wsAuthed = false;
            g_wsClockId = 0;
            g_wsStateId = 0;
            g_stateSubscribed = false;
            ++g_linkStats.connections;
            g_linkStats.lastConnectMs = millis();
            notifyStatus(false);
            Serial.printf("[ha] ws connected at=%lu count=%lu\n",
                          static_cast<unsigned long>(g_linkStats.lastConnectMs),
                          static_cast<unsigned long>(g_linkStats.connections));
            break;
        case WStype_DISCONNECTED:
            g_wsAuthed = false;
            g_wsClockId = 0;
            g_wsStateId = 0;
            g_stateSubscribed = false;
            ++g_linkStats.disconnects;
            g_linkStats.lastDisconnectMs = millis();
            g_linkStats.lastDisconnectTime = haLocalTimeText();
            notifyStatus(false);
            Serial.printf("[ha] ws disconnected at=%lu count=%lu wifi=%d\n",
                          static_cast<unsigned long>(g_linkStats.lastDisconnectMs),
                          static_cast<unsigned long>(g_linkStats.disconnects),
                          networkConnected());
            break;
        case WStype_TEXT:
            wsHandleText(payload, length);
            break;
        case WStype_ERROR:
            ++g_linkStats.errors;
            Serial.println("[ha] ws error");
            break;
        default:
            break;
    }
}

void haBegin() {
    if (g_dataSuspended) return;
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

void haNetworkDown() {
    ws.setSuspended(true);
    if (g_wsStarted) ws.disconnect();
    g_linkStats.lastLoopMs = 0;
    g_wsResumePending = g_wsStarted;
    if (g_wsResumePending) ws.setReconnectInterval(0);
}

void haLoop() {
    pollHttpWork();
    ws.pollConnect();
    if (g_dataSuspended || !networkConnected()) return;
    if (g_wsStarted) {
        ws.setSuspended(false);
        uint64_t now = clockMillis();
        if (g_linkStats.lastLoopMs) {
            uint32_t gap = static_cast<uint32_t>(now - g_linkStats.lastLoopMs);
            if (gap > g_linkStats.maxLoopGapMs) g_linkStats.maxLoopGapMs = gap;
        }
        g_linkStats.lastLoopMs = now;
        ws.loop();
        if (g_wsResumePending) {
            ws.setReconnectInterval(5000);
            g_wsResumePending = false;
        }
    }
    if (g_wsAuthed && !g_stateSubscribed && (int32_t)(millis() - g_stateRetryAt) >= 0) {
        if (g_wsStateId) {
            JsonDocument doc;
            doc["id"] = g_wsMessageId++;
            doc["type"] = "unsubscribe_events";
            doc["subscription"] = g_wsStateId;
            wsSendJson(doc);
        }
        wsSubscribe();
    }
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

bool haStateUpdatesReady() { return haWsConnected() && g_stateSubscribed; }

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
