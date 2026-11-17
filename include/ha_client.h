#pragma once

#include <Arduino.h>

enum class EntityType : uint8_t {
    Unknown,
    Light,
    Switch,
    Climate,
    Scene,
    Media,
    Sensor,
    Cover,
};

struct AuxEntity {
    String id;
    String name;
    String state;
    String unit;
    String detail;
    EntityType type = EntityType::Unknown;
    bool on = false;
    bool available = false;
    int brightness = -1;
};

struct EntityState {
    String id;
    String name;
    String state;
    String unit;
    String detail;
    String customName;
    EntityType type = EntityType::Unknown;
    bool on = false;
    bool available = false;
    int brightness = -1;
    float current = NAN;
    float target = NAN;

    String hvacAction;
    String hvacModes;
    String fanModes;
    String swingModes;
    String fanMode;
    String swingMode;
    String presetModes;
    String presetMode;
    float minTemp = 16.0f;
    float maxTemp = 30.0f;
    float tempStep = 0.5f;

    AuxEntity aux;
};

typedef void (*HaStateCallback)(const EntityState& state);
typedef void (*HaStatusCallback)(bool online);
typedef void (*HaAuthCallback)();

EntityType entityTypeFromId(const String& id);
String defaultNameFromId(const String& id);
String entityDomain(const String& id);
String asciiText(const String& text);

void haBegin();
void haLoop();
bool haWsConnected();
void haSetStateCallback(HaStateCallback cb);
void haSetStatusCallback(HaStatusCallback cb);
void haSetAuthCallback(HaAuthCallback cb);

bool haFetchState(EntityState& entity);
bool haFetchEntityOptions(String& optionsHtml, int maxOptions = 300);
bool haCallService(const String& domain, const String& service,
                   const String& entityId, const String& extraJson = "");
