#pragma once

#include <Arduino.h>

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
    uint8_t brightness = 30;
    uint8_t theme = 0;  // 0 = flipper, 1 = classic

    uint16_t dayStart = 7 * 60;
    uint16_t dayEnd = 21 * 60;
    uint8_t dayBrightness = 5;
    uint16_t dayOffSeconds = 0;

    uint16_t nightStart = 21 * 60;
    uint16_t nightEnd = 7 * 60;
    uint8_t nightBrightness = 5;
    uint16_t nightOffSeconds = 0;
};

extern AppConfig g_config;

void configLoad();
bool configSave();
bool configComplete();
bool haConfigured();

void networkBegin();
void networkLoop();
bool configModeActive();
String networkIp();

void appConfigHandleCommand(const String& line);
