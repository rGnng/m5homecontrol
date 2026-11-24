#pragma once

#include <Arduino.h>

static constexpr const char* FIRMWARE_UPDATE_USER = "admin";
static constexpr const char* FIRMWARE_UPDATE_PASSWORD = "haremote";

static constexpr int GRID_SLOTS = 8;
static constexpr int MAX_PAGES = 12;
static constexpr int MAX_ENTITIES = MAX_PAGES * GRID_SLOTS;

static constexpr uint8_t CLIMATE_COOL = 1u << 0;
static constexpr uint8_t CLIMATE_HEAT = 1u << 1;
static constexpr uint8_t CLIMATE_DRY  = 1u << 2;
static constexpr uint8_t CLIMATE_FAN  = 1u << 3;
static constexpr uint8_t CLIMATE_AUTO = 1u << 4;

struct AppConfig {
    String ssid;
    String pass;
    String hostname = "ha-remote";
    String haUrl;
    String haToken;
    String entities[MAX_ENTITIES];
    String entityNames[MAX_ENTITIES];
    String auxEntities[MAX_ENTITIES];
    String auxNames[MAX_ENTITIES];
    uint8_t climateModes[MAX_ENTITIES] = {};
    String pageTitles[MAX_PAGES];
    uint8_t pageCount = 1;
    uint8_t entityCount = GRID_SLOTS;
    uint8_t theme = 0;  // 0 = flipper, 1 = classic
    bool lowBatteryWarning = true;
    uint16_t lightSleepMinutes = 1;
    uint16_t deepSleepMinutes = 30;

    uint16_t dayStart = 7 * 60;
    uint8_t dayBrightness = 5;
    uint8_t dayDimBrightness = 1;
    uint16_t dayDimSeconds = 0;
    uint16_t dayOffSeconds = 0;

    uint16_t nightStart = 21 * 60;
    uint8_t nightBrightness = 5;
    uint8_t nightDimBrightness = 1;
    uint16_t nightDimSeconds = 600;
    uint16_t nightOffSeconds = 120;
};

extern AppConfig g_config;

void configLoad();
bool configSave();
bool configComplete();
bool haConfigured();

void networkBegin();
void networkLoop();
bool networkSetSuspended(bool suspended);
bool networkSuspended();
bool networkConnected();
bool configModeActive();
String networkIp();
String commandLedsStatus();
String dockInputStatus();
bool dockConnected();
String menuStatus();
String backlightStatus();
String refreshStatus();
bool setDisplayMode(const String& mode);
bool beginDisplayRead();
void endDisplayRead(bool restoreSleep);

void appConfigHandleCommand(const String& line);
