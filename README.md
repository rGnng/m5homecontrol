# Home Control

A Home Assistant remote for the **M5Stack Core Basic v2.7** (ESP32, 320x240).

![Device screen showing the Flipper theme and Japanese and English entity names](docs/images/screen.png)

- M5Code-style menu: eight rows per page
- Two themes: `Flipper` (default) and `Classic`
- BoutiqueBitmap9x9 Regular for menu rows, states, and detail values, with each
  native dot doubled exactly
  (typical CJK ink 18×18 px); see [font details](assets/fonts/README.md)
- Numbered index column, entity name, live state on the right
- Dynamic number of pages, eight entities per page
- Configurable page titles, default `PAGE1`, `PAGE2`, ...
- Live state over the Home Assistant WebSocket API
- Control over the Home Assistant REST API
- Configuration stored in NVS
- Web configuration with an entity selector loaded from Home Assistant
- Normal/dim/off display states with day/night schedules and web previews
- M5Code arrow/select sound effects through the built-in speaker
- Value-area refreshes leave unchanged labels and rows untouched
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
| 7 |                                   |
| 8 |                                   |
+--------------------------------------+
```

- Menu starts at y = 24
- Row height = 27 px, eight rows per page
- Left index column: 24 px
- Index digits are centered by their visible pixels, above the row divider
- Font cell starts 2 px below the row top; typical Latin ink starts at 6 px
- Flipper theme: orange rows, dark index text; the selected row and index cell
  use a medium brown background with cream text
- Classic theme: dark blue selected row with green text
- Row divider: 1 px solid line in Classic, dotted in Flipper
- No bottom hint bar
- Entity state is right-aligned at x = 314
- Temperature units use `C` / `F` without the degree circle, including sensor
  and AC AUX values
- Custom display name overrides the Home Assistant friendly name; empty uses the
  HA friendly name
- Climate entities show the actual HA mode: `HEAT`, `COOL`, `AUTO`, `DRY`,
  `FAN`, `OFF`, plus the target temperature. If no target is available, the menu
  falls back to the measured temperature; AC detail's CURRENT row shows the
  measured room temperature

## Status bar

- Left: current page title
- Center: page squares, active page highlighted
- Right: combined Wi-Fi / HA link icon, then the battery icon
- Wi-Fi is not configured: dim signal icon, blinking
- While connecting, retrying, or disconnected: the signal icon blinks; no red X
- The icon follows Wi-Fi and the authenticated HA WebSocket; a failed entity
  command does not change connection status
- Confirmed Core USB charging: bolt inside the battery icon
- Assumed dock/external power: plug overlay; charging/full status remains unknown
- Unavailable battery reading: `?` until the dock hint activates
- No clock, no battery percentage
- The battery icon is monochrome, same color as the signal bars

## Dynamic pages and titles

Pages are explicitly configured in the web UI. Use **Add page** and
**Remove** in the backend; use **↑ / ↓** beside each editable title to reorder
whole pages, then **Save and reboot**. The device shows exactly the configured number
of pages, including empty pages.

- Eight slots per page
- Maximum 12 pages / 96 slots
- Page title defaults to `PAGEn` (no space)
- Empty slots stay in place; pages are not compacted

See [Controls](#controls) for page switching and cursor navigation.

## Battery and power

The Faces Bottom3 side LEDs briefly sweep from bottom to top on both sides when
the device sends a Home Assistant service command. The sweep uses the active
theme's `commandLedColor` (Flipper orange / Classic green), follows current screen
brightness including the day/night schedule, and turns off after 300 ms.
It runs independently of HTTP, so slow HA responses do not freeze it. A new
command restarts the sweep. Polling and incoming state updates do not trigger it.

A black frame clears any latched LED color at startup. `/diag` reports command
animation readiness, brightness (0-255), duration, theme color, active state,
trigger count, and the last sweep's frame count and visited-pixel mask
(`1023` means all ten pixels).

The on-screen battery icon remains. On Core Basic, three consecutive unavailable
PMIC samples show a plug over the battery: assumed dock/external power, with
charging status unknown. A responding PMIC clears this hint immediately.
This user-selected heuristic can also trigger on an I2C fault. Before it activates,
an unavailable battery reading shows `?`, never an empty battery.
Power is sampled every 500 ms, so the hint takes roughly 1–1.5 seconds to activate.

The Bottom3 has its own TP4057 charger and a separate battery power switch.
The Core's IP5306 readings do not directly report the TP4057 charging state.
The four dock contacts are **J3**, wired as GND, SYS_5V, GPIO21/SDA, and
GPIO22/SCL. **J4 is the separate battery connector**, not the dock pinout.
Dock SYS_5V feeds the TP4057 charger and, through isolation diode D1, BUS_5V
for the running device. The TP4057 charges BAT; with the battery switch on,
BAT_ON feeds the Core's battery connection. Off the dock, the Core's IP5306
boosts that battery supply to the system 5V rail. The dock therefore bypasses
the IP5306's USB input; it does not physically disconnect the IP5306.
The keypad shares M5Unified's I2C driver with the PMIC. `/diag` includes the bus
pins and IP5306 registers `00`, `01`, `70`, `71`, and `78` for power troubleshooting.
It also reports the current page and focus. It does not read the ESP32 Hall
sensor: `hallRead()` changes GPIO39 (Button A) into analog mode and can cause a
phantom button press.

Espressif withdrew the built-in Hall sensor as a supported feature because it
does not work properly; see
[PCN20221202](https://documentation.espressif.com/PCN20221202%20Remove%20Hall%20Sensor%20from%20ESP32%20Series%20of%20Documentation.pdf).
The firmware contains no Hall-sensor sampling or experimental Hall endpoint.

The firmware retries an unavailable PMIC, including after startup on the dock.
After three failed samples it reinitializes the shared I2C bus and retries the
PMIC, at most once per ten seconds. When the IP5306 responds, its boost keep-on
bit is enabled to keep the battery output ready while dock power bypasses it.
The manager also verifies boost enable, Core charge enable, and automatic boost
handoff when the Core input is removed. Register changes use read-modify-write;
charger voltage/current, low-battery cutoff, and reserved bits are preserved.
A PMIC already asleep and not
responding to I2C may still need a single press of the red power button or USB
power to wake it.

Docked telemetry may still become unavailable even with boost keep-on enabled.
In the device test, battery handoff continued working and live readings returned
after undocking. A plug means assumed dock power, not confirmed charging/full;
`?` means no current battery reading and does not mean empty.

Web -> Display -> **Refresh battery status**, or `GET /power`, reports the
battery percentage, battery voltage, USB-input voltage, and Core USB charger.
Voltage fields show `Unavailable` when the hardware does not expose a reading.
In particular, the Core Basic's IP5306 has no readable battery/VBUS voltage;
a percentage reading is not converted into a made-up voltage.
`/diag` uses `charging=unknown` when there is no confirmed charging/full signal;
an inactive Core charger is not proof that the dock is not charging.
It also reports `dock_inferred`, `pmic_online`, `core_keep_on`, `core_boost_enabled`,
`core_charge_enabled`, `core_battery_handoff`, and `power_recoveries`.
The Bottom3 TP4057's `CHRG` and `STDBY` pins are unconnected in the
[manufacturer schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1273/A168_Faces_Bottom3_Sche.pdf),
so actual dock charging detection requires a hardware status connection.

## Backlight schedule

Configure **Display** in the web page. Schedule times follow
[Home Assistant's local time](#home-assistant-time).

| Default period | Local hours | Normal brightness | Idle before dim | Dim brightness | Time in dim before off |
| --- | --- | --- | --- | --- | --- |
| Day | 07:00–21:00 | 5% | Disabled | 1% | Disabled |
| Night | 21:00–07:00 | 5% | 10 minutes | 1% | 2 minutes |

The night defaults turn the display off after 12 minutes without input. The form
uses seconds: 600 before dim and another 120 before off. Set a timeout to `0` to
disable it. If dimming is disabled, the off timeout counts from the last input.

| Display state | Appearance | First device key press |
| --- | --- | --- |
| Normal | Selected theme and scheduled brightness | Performs its action and resets idle time |
| Dim | Black background, faint grey text, subdued selection and icons | Restores the normal view and performs its action |
| Off | Backlight off | Wakes the display only; release and press again to act |

These rules cover the Core buttons, keypad, and serial navigation keys. A Core
A/B/C press used to wake an off display is suppressed until release; holding B
cannot open a detail panel during that wake press.

Incoming HA values, clock updates, and day/night transitions do not wake or
brighten a dim/off display. Background scheduling may make it darker or turn it off.
Web **Wake**, **Dim**, and **Off** preview the display using saved brightness
settings without saving or running an entity action. Device input clears the
preview, resets idle time, and restores the normal view.

## Home Assistant time

The firmware subscribes to an HA `now()` template over the authenticated
WebSocket connection. HA supplies the UTC timestamp, current UTC offset, and
timezone name immediately after authentication and at each minute boundary.
Time updates are pushed by HA; there is no periodic REST time poll or NTP server.

- HA's configured timezone and current offset determine the day/night schedule,
  including daylight saving and timezone changes while connected
- A 64-bit monotonic clock advances time between updates and while disconnected
- While offline, the last known offset is retained; timezone/DST changes are
  learned after reconnection, when a fresh clock subscription is created
- Failed or invalid clock updates trigger a subscription retry after 30 seconds;
  a subscription with no valid update for three minutes is recreated
- Until the first valid time sample, the configured fallback brightness is used
  and automatic dim/off timers wait for the clock

The web **Display** section shows HA local time and timezone as a snapshot when
the page loads. `/diag` exposes the clock source and sample age; see
[Web endpoints](#web-endpoints) for the diagnostic fields.

## Sound

Menu movement plays `sfx_arrow`; actions play `sfx_select`. Both are the
original M5Code WAV effects played through the built-in Core speaker via
`M5.Speaker`.
Docking and undocking are silent.

## Home Assistant setup

A **long-lived access token** is required even when the dashboard itself allows
anonymous viewing.

1. Home Assistant -> profile -> Security -> Long-lived access tokens
2. Create a token
3. Open the device web page and paste the token

Enter your Home Assistant base URL, such as `http://<ha-host>:8123`.

### Entity selector

When a token is configured and Wi-Fi is up, the web page downloads
`/api/states` and fills a shared `<datalist>`. Start typing in an entity field
or open the list. Manual entity IDs still work if the list cannot be loaded.
The entity list is cached for 60 seconds.

## Web configuration

- No saved Wi-Fi SSID: open AP `HA-Remote`, then `http://192.168.4.1`
- Normal operation: `http://<device-ip>/` or `http://ha-remote.local/`
- With saved Wi-Fi credentials, the device keeps retrying a lost connection

Fields:

- Theme: `Flipper` (default) or `Classic`
- Fallback brightness, 1-100%, default 30%
- Day/night normal/dim brightness, dim/off timeouts, and live Wake/Dim/Off previews
- Dynamic page blocks: title + eight slots
- Each slot: entity ID and display name
- Climate slots only: optional AUX entity/name and allowed MODE values (COOL/HEAT/DRY/FAN/AUTO)
- Hostname used for `http://<hostname>.local/`, including web firmware updates
- Wi-Fi SSID and password
- Home Assistant base URL
- Long-lived access token

The page is plain: square inputs and buttons, no rounded corners. Empty slots
and empty pages are kept as configured.
After saving, the page waits for the device to restart and reloads automatically.
If reconnection times out, it reports the problem and enables Save again.
The **Download screenshot** link beside **Update firmware** saves the current
320×240 display as a BMP. It streams one row at a time using about 1 KB of
working memory. Capturing does not wake the display or change pages.

### Web endpoints

| Method and path | Purpose |
| --- | --- |
| `GET /` | Configuration form |
| `POST /save` | Save settings and reboot; the web form reconnects automatically |
| `POST /display` | Preview the display with form field `mode=wake`, `dim`, or `off`; settings are not saved |
| `GET /power` | Current battery and Core charger readings |
| `GET /diag` | Menu, backlight, LED, and power diagnostics; reading it does not wake the display |
| `GET /screenshot.bmp` | Stream the current display as `ha-remote-screen.bmp`; does not redraw or wake the display |
| `GET /update` | Firmware upload form; requires the firmware username/password |
| `POST /update?size=<bytes>` | Authenticated firmware upload using multipart field `firmware` |

Backlight diagnostics include `backlight=normal|dim|off`, `backlight_pwm` (0–255),
`dim_theme` (0/1), and `display_override` (-1 for automatic, 1 for Dim preview,
2 for Off preview).
Clock diagnostics include `time_source=ha|unavailable`, `ha_timezone`,
`utc_offset` (seconds east of UTC), `local_time`, and `ha_clock_age` (seconds
since the last valid sample). Reading diagnostics does not count as activity.

Connection diagnostics include `wifi_connected`, `ws_connected`, `ws_connections`,
`ws_disconnects` (disconnect events, including failed connection attempts),
`ws_authentications`, `ws_reconnects` (authentications after the first), and
`ws_errors`. `uptime_ms`, `ws_last_connect_ms`, `ws_last_disconnect_ms`, and
`ws_last_auth_ms` use milliseconds since boot; zero event timestamps mean no
event has been recorded. `ws_last_disconnect_time` also reports the disconnect
in HA local time when available. `ws_loop_max_gap_ms` records the longest gap
between WebSocket processing calls.

`ha_command_failures`, `ha_last_command_status`, and `ha_last_command_ms` report
service errors separately. Status is an HTTP code, a negative transport error,
or zero when no request ran. Counters reset on reboot.

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

The following actions follow the [dim/off wake rules](#backlight-schedule).
Only AC (`climate.*`) items have a detail panel; sensors are read-only.

### Core A/B/C buttons

| Button | Main menu | AC detail |
| --- | --- | --- |
| A | Cursor up | Cursor up; decrease the draft temperature while editing |
| B, short press | Toggle/run the selected entity; cycle an AC's allowed modes and OFF | Activate the selected control or confirm a temperature edit |
| B, hold for 600 ms | Open the selected AC's detail panel | Discard an unconfirmed edit and return to the main menu |
| C | Cursor down | Cursor down; increase the draft temperature while editing |

Cursor movement wraps within the current page and skips empty menu slots.

### FACES Calculator3 keypad

| Physical key | Action |
| --- | --- |
| r1c1 (AC), short or long press | Return to the main menu, discarding an unconfirmed temperature edit |
| r1c2 (M) / r1c3 (%) | Previous / next page, wrapping at either end; main menu only |
| 1–8 | Select and run that main-menu slot; in AC detail, 1–7 only focus a control |
| r3c4 (+) / r4c4 (−) | Increase / decrease the selected adjustable value |
| 0 | Cancel a temperature edit; otherwise return to the main menu |
| r5c4 (=), short or long press | Open AC detail if available; otherwise toggle/run the selected entity. In AC detail, activate the control or confirm an edit |

Number positions are remapped on the physical keypad:

```text
       c1   c2   c3   c4
r2      1    2    3
r3      4    5    6    +
r4      7    8    9    -
```

There are eight menu slots, so 9 has no assigned slot.

### Adjusting values

- Main-menu AC: `+`/`-` changes target temperature by HA's `target_temp_step`,
  within `min_temp`/`max_temp`. The displayed setpoint updates after success
- Main-menu dimmable light: changes brightness by 10 percentage points within
  0–100%; reaching 0% turns it off
- AC detail: adjusts the highlighted TEMP or dimmable-light AUX value. TEMP
  changes are immediate unless an explicit draft edit is active
- On/off-only lights, unavailable entities, and non-adjustable selections ignore
  these keys. No command is sent when already at the requested limit

### Serial navigation

Serial keys use logical digits without the physical keypad remap:
`1`–`8` select/run a slot, `w`/`s` move the cursor, `p`/`n` (or `M`/`%`)
change pages, `e` activates, `=` opens AC detail or runs the selection,
`.` opens AC detail, `0` goes back/cancels, and `+`/`-` adjusts values.

## AC detail

Long press B on a `climate.*` row opens a detail menu with eight numbered rows:

```
1  MODE       COOL
2  TEMP       24.0C
3  FAN        AUTO
4  SWING      OFF
5  ACTION     COOLING
6  <AUX name> ON
7  CURRENT    27.5C
8
```

- MODE cycles OFF plus the allowed, HA-supported modes configured in the web UI
- TEMP enters draft edit mode; use the [controls above](#controls) to adjust,
  confirm, or cancel it
- FAN and SWING cycle the HA `fan_modes` / `swing_modes`
- ACTION and CURRENT are read-only
- AUX uses its configured display name; lights/switches toggle, scenes run,
  media players play/pause, covers toggle, and sensors are read-only. Dimmable
  light values include the brightness percentage. The cursor skips this row
  when no AUX entity is configured
- Rows use uppercase text
- The status bar shows the AC display name and hides page squares
- Missing values show `--`
- Row 8 stays visible with its number, but has no label or action and is skipped by cursor navigation
- There is no Back row; navigation uses the controls above

The [official Calculator3 protocol](https://docs.m5stack.com/en/protocol/A005-V3/I2C)
defines r1c1 as ASCII `A` on a short press and backspace on a long press.
Key data is read directly from I2C address `0x08`, without a register address.

Value refreshes in the menu and AC panel redraw only changed text bounds;
unchanged values are skipped. Shorter values clear their previous width so no
old characters remain. Labels redraw only when their text, color, or truncation
changes. Selection, edit highlighting, and page changes redraw the affected rows.
Full-screen and panel-transition clears use black before painting the theme.
The full screen redraws at boot and when entering/leaving the dim palette.

## Home Assistant API

- `GET /api/states/<entity_id>` initial values
- `GET /api/states` entity selector
- REST service calls for control
- WebSocket `ws://<host>:8123/api/websocket` for `state_changed`
- WebSocket `render_template` subscription for [HA time](#home-assistant-time)
- Initial resync after WebSocket authentication and every five minutes
- REST resync fetches all configured entities and climate AUX entities; only
  visible rows redraw. All configured entities receive WebSocket updates
- HTTP requests, including the web entity selector, run on a worker while the
  main thread continues WebSocket processing. Actions still wait for their HTTP
  result; live state events received during a REST fetch take precedence over
  that snapshot

## Build and flash

### Default flashing credentials

| Flash method | Username | Password |
| --- | --- | --- |
| Web OTA (`/update`) | `admin` | `haremote` |
| USB | Not required | Not required |

The web credentials are `FIRMWARE_UPDATE_USER` and `FIRMWARE_UPDATE_PASSWORD`
in [include/app_config.h](include/app_config.h).

### Build and upload

```sh
pio run
pio run -t upload --upload-port "<serial-port>"
pio device monitor -p "<serial-port>" -b 115200
```

After the first USB flash, update over Wi-Fi through the web firmware page.
The configured hostname (`ha-remote` by default) is advertised over mDNS for
access at `http://<hostname>.local/`.

### HTTP firmware updates

Open **Update firmware** on the configuration page, or go directly to
`http://<device-ip>/update`.

- Username: `admin`; password: `haremote`
- Select `.pio/build/m5stack-core/firmware.bin`, then **Upload and restart**
- Progress and upload errors appear on the page; only a successful update restarts
- Existing configuration is kept
- HTTP uploads use the existing web port, TCP 80; no UDP or callback port is needed

Command-line upload (replace `<device-ip>` with your device's address):

```sh
firmware=.pio/build/m5stack-core/firmware.bin
curl --fail-with-body --user admin:haremote \
  -H 'Expect:' \
  -F "firmware=@$firmware" \
  "http://<device-ip>/update?size=$(wc -c < "$firmware" | tr -d ' ')"
```

Use USB for the first installation if the device has no `/update` endpoint.

## Files

```
platformio.ini
src/main.cpp          menu UI, partial refresh, input
src/app_config.cpp    NVS, network, web server, serial console
src/ha_client.cpp     REST + WebSocket client
src/power_status.cpp  shared power sampling, PMIC recovery, battery handoff
include/app_config.h
include/backlight_schedule.h
include/keypad_controls.h  physical keypad remap
include/ha_client.h
include/ha_clock.h    HA clock and local-time conversion
include/power_status.h
```

Host checks (no device required; control/WebSocket checks use the ArduinoJson
dependency installed by PlatformIO):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_power_status.cpp -o /tmp/m5-power-status-test
/tmp/m5-power-status-test
g++ -std=c++17 -Wall -Wextra -Werror -Itools/test_support -Iinclude \
  src/power_status.cpp tools/test_power_recovery.cpp -o /tmp/m5-power-recovery-test
/tmp/m5-power-recovery-test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_backlight_schedule.cpp -o /tmp/m5-backlight-test
/tmp/m5-backlight-test
python3 tools/test_controls.py
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/ui_font_data.cpp tools/test_ui_font.cpp -o /tmp/m5-ui-font-test
/tmp/m5-ui-font-test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_ha_clock.cpp -o /tmp/m5-ha-clock-test
/tmp/m5-ha-clock-test
python3 tools/test_ha_clock_ws.py
python3 tools/test_ha_http_worker.py
```
