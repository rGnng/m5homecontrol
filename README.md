# Home Control

A Home Assistant remote for the **M5Stack Core Basic v2.7** (ESP32, 320x240).

- M5Code-style menu: eight rows per page
- Numbered index column, entity name, live state on the right
- Dynamic number of pages, eight entities per page
- Configurable page titles, default `PAGE1`, `PAGE2`, ...
- Live state over the Home Assistant WebSocket API
- Control over the Home Assistant REST API
- Configuration stored in NVS
- Web configuration with an entity selector loaded from Home Assistant
- Display brightness setting plus day/night backlight schedule
- M5Code arrow/select sound effects through the built-in speaker
- Partial refresh only
- No boot/setup screen: the menu is shown immediately

## Layout

```
+--------------------------------------+
| PAGE1        ####          )) []      |  status bar 24 px
+--------------------------------------+
| 1 | Living room light      ON 80%     |
| 2 | Desk lamp              OFF        |
| 3 | Air conditioner        COOL 24.0C |
| 4 | TV                     PLAYING    |
| 5 | Movie night            SCENE      |
| 6 | Indoor temperature     23.4C      |
| 7 | EMPTY                             |
| 8 | EMPTY                             |
+--------------------------------------+
```

- Menu starts at y = 24
- Row height = 27 px, eight rows per page
- Left index column: 24 px, dark for configured entities, teal for empty rows,
  green when selected
- Selected row: navy background, green text
- 1 px dark grey divider between rows, including the index column
- No bottom hint bar
- Entity state is right-aligned at x = 314
- Custom display name overrides the Home Assistant friendly name; empty uses the
  HA friendly name
- Climate entities show the actual HA mode: `HEAT`, `COOL`, `AUTO`, `DRY`,
  `FAN`, `OFF`, plus the current temperature when available

## Status bar

- Left: current page title
- Center: page squares, active page white
- Right: combined Wi-Fi / HA link icon, then the battery icon
- Wi-Fi is not configured: grey signal icon, blinking
- Wi-Fi down or HA disconnected: red X over the signal icon
- Charging: neutral grey bolt in the battery icon, no bright fill
- No clock, no battery percentage

## Dynamic pages and titles

Pages are explicitly configured in the web UI. Use **Add page** and
**Remove page** in the backend; the device shows exactly the configured number
of pages, including empty pages.

- Eight slots per page
- Maximum 12 pages / 96 slots
- Page title defaults to `PAGEn` (no space)
- Empty slots stay in place; pages are not compacted
- Page 1 can have three entities and five empty slots; page 2 stays independent

Page switching:

- Long press **A** / **C** while the menu is visible
- Serial `p` / `n`

## Backlight schedule

Configured in the web page under **Display**.

- `day`: 07:00-21:00, brightness 5%, screen off disabled by default
- `night`: 21:00-07:00, brightness 5%, screen off disabled by default
- Each period has an idle/wake brightness and a screen-off timeout in seconds
- `0` seconds means the screen never turns off
- Any button or serial key wakes the screen and resets the timer
- When a timeout is configured, the screen turns off that many seconds after
  the last input

The fallback brightness is used until network time is available.

## Sound

Menu movement plays `sfx_arrow`; actions play `sfx_select`. Both are the
original M5Code WAV effects played through the built-in Core speaker via
`M5.Speaker`.

## Home Assistant setup

A **long-lived access token** is required even when the dashboard itself allows
anonymous viewing.

1. Home Assistant -> profile -> Security -> Long-lived access tokens
2. Create a token
3. Open the device web page and paste the token

The base URL is pre-filled with `http://10.0.0.11:8123`.

### Entity selector

When a token is configured and Wi-Fi is up, the web page downloads
`/api/states` and fills a shared `<datalist>`. Start typing in an entity field
or open the list. Manual entity IDs still work if the list cannot be loaded.
The entity list is cached for 60 seconds.

## Web configuration

- First boot / no Wi-Fi: open AP `HA-Remote`, then `http://192.168.4.1`
- Normal operation: `http://<device-ip>/` or `http://ha-remote.local/`

Fields:

- Brightness, 1-100%, default 30%
- Dynamic page blocks: title + eight slots, each with entity ID and optional display name, with add/remove page
- Wi-Fi SSID and password
- Home Assistant base URL
- Long-lived access token

The page is plain: square inputs and buttons, no rounded corners. Empty slots
and empty pages are kept as configured.

## Serial configuration

`115200` baud, commands start with `:`.

```
:wifi <ssid> <password>
:ha <url> [token]
:entity <1-96> <entity_id>
:name <1-96> <display name>
:title <1-12> <name>
:bright <1-100>
:show
:clear
:reboot
:help
```

`:clear` erases the NVS namespace, including Wi-Fi credentials.

## Controls

| Input | Action |
| --- | --- |
| A / C | move focus up / down |
| A / C long press | previous / next page |
| B | run the focused entity |
| B long press | open detail view |
| 1-8 | direct hotkey for the current page |
| A / C in detail | brightness or target temperature |
| B in detail | back |

Serial developer keys: `1-8`, `w`/`s` focus, `p`/`n` page, `e`/Enter action,
`0` back, `M` detail, `+`/`-` adjust.

Only the changed row, detail content, or status bar is redrawn. The full screen
is painted once at boot.

## Home Assistant API

- `GET /api/states/<entity_id>` initial values
- `GET /api/states` entity selector
- REST service calls for control
- WebSocket `ws://<host>:8123/api/websocket` for `state_changed`
- Initial resync after WebSocket authentication and every five minutes
- Only the current page is polled; all configured entities receive WebSocket
  updates

## Build and flash

```sh
pio run
pio run -t upload --upload-port /dev/cu.usbserial-5A6C0461241
pio device monitor -p /dev/cu.usbserial-5A6C0461241 -b 115200
```

## Files

```
platformio.ini
src/main.cpp          menu UI, partial refresh, input
src/app_config.cpp    NVS, network, web server, serial console
src/ha_client.cpp     REST + WebSocket client
include/app_config.h
include/ha_client.h
```
