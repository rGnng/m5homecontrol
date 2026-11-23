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
- Configuration HTML streams in small chunks, including the control script,
  without allocating the entire page in RAM
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
- While normal and connecting, retrying, or disconnected: the signal icon blinks; no red X
- While undocked and Dim, the link and battery readings freeze; the battery's
  plug overlay still follows GPIO26. Docked Dim retains live values and status updates
- The icon follows Wi-Fi and the authenticated HA WebSocket; a failed entity
  command does not change connection status
- Confirmed Core USB charging: bolt inside the battery icon
- PC817C reports base power on GPIO26: plug inside the battery icon; charging power is available, but full status remains unknown
- Unavailable battery reading while undocked: `?`
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

Docking lights all ten LEDs together, fading from red through yellow to green over 350 ms
at 15% peak LED brightness, fading in over the first 70 ms and out over the last
70 ms. This brightness is independent
of the screen setting. Dock feedback finishes before a queued HA command sweep.

A black frame clears any latched LED color at startup. `/diag` reports command
animation readiness, brightness (0-255), duration, theme color, active state,
trigger count, and the last sweep's frame count and visited-pixel mask
(`1023` means all ten pixels).

The on-screen battery icon shows a plug whenever the PC817C pulls GPIO26 LOW,
indicating that the base is powered and battery charging power is available,
even when battery telemetry is available. Loss of that signal removes the plug;
an unavailable battery reading shows `?`, never an empty battery. The screen's
dock indication follows the 50 ms contact debounce independently of PMIC polling.
Power is sampled every 500 ms while active and every 5 seconds while idle.

The Bottom3 has its own TP4057 charger and a separate battery power switch.
The Core's IP5306 readings do not directly report the TP4057 charging state.
The factory four dock contacts are **J3**, wired as GND, SYS_5V, GPIO21/SDA, and
GPIO22/SCL. The completed [GPIO26 modification](#gpio26-dock-detection) repurposes
one contact as SENSE. **J4 is the separate battery connector**.
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
PMIC, at most once per ten seconds while active or once per minute while idle.
When the IP5306 responds, its boost keep-on
bit is enabled to keep the battery output ready while dock power bypasses it.
The manager also verifies boost enable, Core charge enable, and automatic boost
handoff when the Core input is removed. Register changes use read-modify-write;
charger voltage/current, low-battery cutoff, and reserved bits are preserved.
A PMIC already asleep and not
responding to I2C may still need a single press of the red power button or USB
power to wake it.

Docked telemetry may still become unavailable even with boost keep-on enabled.
In the device test, battery handoff continued working and live readings returned
after undocking. The screen's plug means base power and charging power are available, without confirming charging current or full status;
`?` means no current battery reading and does not mean empty.

Web -> **Power** shows a battery icon; **Refresh** updates its fill level.
An unavailable reading shows `?`; hovering the icon reveals its status or
approximate percentage. `GET /power` retains the text reading, while
`GET /power?format=icon` returns the icon. Core Basic's IP5306 does not expose
battery/VBUS voltage readings.
`/diag` uses `charging=unknown` when there is no confirmed charging/full signal;
an inactive Core charger is not proof that the dock is not charging.
It also reports `dock_inferred`, `pmic_online`, `core_keep_on`, `core_boost_enabled`,
`core_charge_enabled`, `core_battery_handoff`, and `power_recoveries`.
The Bottom3 TP4057's `CHRG` and `STDBY` pins are unconnected in the
[manufacturer schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1273/A168_Faces_Bottom3_Sche.pdf),
so the added PC817C reports base power and charging availability, rather than
measuring charge current or the TP4057's charging/full state.

### Low battery warning

Web **Power → Low battery warning** enables or disables the warning; it is on
by default and saved with **Save and reboot**. While undocked with a known
battery level strictly below 25%, all ten LEDs flash red twice as soon as the
LEDs are free, then repeat at one-minute intervals. Each flash lasts 80 ms
with an 80 ms gap, at 15% LED brightness
independent of the display. Docking, disabling the setting, an unknown reading,
or a reading of 25% or higher suppresses the warning. User command and dock
animations take priority.

The Core Basic gauge reports only 0%, 25%, 50%, 75%, or 100%, so its lowest
0% bucket triggers this strict threshold. Warnings work in Normal, Dim, and
Off/light sleep without waking the screen or reconnecting Wi-Fi. Deep sleep
remains uninterrupted; there are no added timer wakes. `/diag` reports
`low_battery_warning`, `low_battery_eligible`, and `low_battery_warnings`.

### Connected power saving

While the PC817C reports base power, the ESP32 stays awake at its active CPU speed with
Wi-Fi modem sleep disabled, HA connected, and live updates running. The display
still follows the normal day/night Dim and Off schedule; only the LCD sleeps
when the screen is Off. Web configuration, screenshots, and OTA remain reachable.
Picking up the device wakes the normal view and restarts its idle timers.

**The following device power-saving rules apply only while undocked.** The
GPIO26 input determines docking; PMIC inference does not change this policy.
In normal display mode, the remote accepts live HA updates. Dim and off suspend
HA data, show `--` for entity values, and freeze the status bar. Dim keeps Wi-Fi
associated so web configuration, screenshots and OTA remain reachable. On Core
Basic, Off stops Wi-Fi and uses the active day/night sleep-mode setting. Light
sleep resumes on a keypad or Core A/B/C key; deep sleep boots the remote on a
keypad key release. Web and OTA are unavailable while asleep.

- Normal display mode disables Wi-Fi modem sleep, keeping the receiver awake
  even when the CPU is idle. Wi-Fi requests the SDK's maximum TX cap (20 dBm),
  subject to the radio's PHY limits. RX has no independent power-level setting.
  Dim uses maximum modem sleep with a listen interval of ten AP beacon
  intervals; the TX cap stays high to preserve the link. Larger HTTP transfers
  temporarily disable modem sleep, then restore the current display policy.
  Web access can have higher latency in dim. These radio settings do not
  change the CPU idle policy.
- Entering dim/off disconnects the HA WebSocket, stops HA reconnects, heartbeats,
  subscriptions and REST reads, and cancels queued commands and entity-list
  downloads. An HTTP request already running may finish, but its result is
  discarded even if the display wakes before it returns.
- Values in all configured menu rows and AC detail controls become `--`, with
  names retained. The dim screen is painted once and stays static. Entering off
  directly also clears displayed values before the LCD sleeps.
- Waking restores normal networking, reconnects HA, and refreshes the visible
  page before other pages. Each value stays `--` until fresh data arrives;
  controls for stale or unavailable entities do not send commands.
- After 15 seconds without input, or when the display is dim/off, CPU speed
  drops to 80 MHz. Input and sounds restore 160 MHz. Both speeds share the
  ESP32's 320 MHz PLL, selected before Wi-Fi starts. HTTP work, web
  configuration, and screenshots temporarily restore that
  speed; firmware uploads keep it restored until upload completion.
- Display off sends the LCD controller to sleep as well as setting backlight
  brightness to zero. Wake paints `--` before lighting the screen; asynchronous
  HA results then fill in the values.
  Off waits for HTTP work, firmware uploads, sounds and LED animations to finish
  before stopping the network and sleeping. GPIO35 (keypad) and GPIO39/38/37
  (Core A/B/C) wake the CPU in light sleep. With an unverified/faulty keypad IRQ, one-second
  timer wakes poll for keys; with a verified IRQ, health wakes run every minute.
  Timer wakes leave the screen and Wi-Fi off. A keypad wake can take up to one
  second with the polling fallback. Sleep setup failures retain input polling.
  GPIO26 changes also wake light sleep for debounced dock detection. Deep sleep
  uses GPIO35/EXT0 for the keypad and GPIO26/EXT1 for dock changes, with no timer
  wakes. If the keypad is missing or
  its IRQ is faulty, Off falls back to light sleep. Other board models retain
  the LCD-only Off behavior.
- The speaker task and DAC stop one second after a sound finishes. GPIO25 is
  driven low on Core Basic. This quiets the signal path; the board does not
  provide a separate amplifier shutdown control.
- Calculator3 key reads use its active-low GPIO35 IRQ after a real key verifies
  the low/read/high signal cycle. Idle health polling then runs once per second.
  Active polling remains 20 ms. Unverified, missing or faulty IRQ wiring retains
  100 ms idle polling; a key found by health polling restores this fallback
  automatically.
- Day/night backlight calculations run once per second. Dim/off timers run at
  their exact deadlines; button input, web previews and the first HA clock sync
  apply immediately.
- PMIC sampling slows from 500 ms to 5 seconds while idle. Explicit battery
  refresh bypasses the sample cache.
- While normal, HA sends state updates only for configured primary and AUX
  entities. Commands queue a delayed REST check of matching slots, rather than
  every page. Full
  reconciliation runs every 30 minutes while subscribed, or every 30 seconds
  if live state updates are unavailable. Failed reads retry after 15 seconds.

`/diag` reports `ha_data_suspended`, `ha_data_generation`, `ha_http_discarded`,
`entity_values_ready`, `aux_values_ready`, `wifi_listen_interval`,
`wifi_rssi_dbm` (-127 while disconnected), `wifi_tx_power_qdbm` (quarter-dBm;
80 means 20 dBm, -1 means unavailable),
`power_idle`, actual `cpu_mhz`, `wifi_sleep`, `lcd_sleep`,
`audio_quiet`, `network_suspended`, `light_sleep_entries`, `light_sleep_ms`,
`light_sleep_errors`, `light_sleep_last_wake`, polling intervals, REST counters,
`ws_state_ready`, watched entity count, state events, and subscription errors. Power saving on the dock can be
verified through these states, but battery runtime or current savings need an
off-dock runtime test or a current meter. Sleep counters can be read after
waking; diagnostics cannot be fetched while Wi-Fi is off.

`keypad_irq_verified`, `keypad_irq_reads`, `keypad_reads` and `keypad_irq_faults`
show whether IRQ mode is working. `backlight_clock_checks` counts time-of-day
calculations; `backlight_checks` also includes idle deadlines and manual input.
The IRQ pin follows the Calculator3's `G35_INT` net in its
[manufacturer schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1272/A005-V3-Faces_Calculator3_Sche.pdf).

## GPIO26 dock detection

The dock uses a **PC817C optocoupler** to signal when dock power is present.
The original **C contact** carries this signal to GPIO26. A physically seated
remote on an unpowered base does not count as powered/docked.

| Contact | Factory function | Modified function |
| --- | --- | --- |
| G | Ground | Ground, unchanged |
| 5 | Dock 5V | Power and charging supply, unchanged |
| D | SDA / GPIO21 | Unchanged |
| C | SCL / GPIO22 | Repurposed as the GPIO26 sensing contact |

Factory connections are documented in the
[Bottom3 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1273/A168_Faces_Bottom3_Sche.pdf).

### Dock side

The intended optocoupler connections are:

```text
Dock 5V -- 910 Ω resistor -- PC817C LED input
Dock GND ------------------- PC817C LED return

PC817C collector -- dock C contact
PC817C emitter   -- dock GND
```

PC817 pin functions are **1: anode, 2: cathode, 3: emitter, 4: collector**.
See the [Sharp datasheet](https://global.sharp/products/device/lineup/data/pdf/datasheet/PC817XxNSZ1B_e.pdf).
The series resistor is **910 Ω**, confirmed by the builder. The taped-over
optocoupler connections are not readable in the photos, so those remain unconfirmed.

### Bottom3 side

Under the battery, **only the C conductor of the G–5–D–C ribbon cable is cut**.
This isolates the dock's C contact from its original GPIO22/SCL connection.

An added wire connects the **contact side of C to GPIO26** on the lower board.
The shared I2C clock connection between the Core, keypad, and PMIC remains intact.

```text
Dock C contact
      |
Bottom3 C contact -- added wire -- GPIO26
      |
      X  C ribbon conductor cut under battery
      |
Original GPIO22 / SCL connection
```

GPIO26 uses its internal pull-up:

- **Powered dock connected:** the optocoupler pulls GPIO26 LOW
- **Dock disconnected or unpowered:** GPIO26 reads HIGH

The earlier direct **G–C test jumper** is replaced by the optocoupler output.
Port B's **O** signal is also GPIO26. The dock's power and common ground
connections remain in use; only the C dock-contact branch is repurposed.

### Firmware behavior

| GPIO26 level | Meaning |
| --- | --- |
| LOW | PC817C indicates base power; battery charging power is available |
| HIGH | No base-power signal; this does not rule out separate Core USB charging |

A stable LOW for 50 ms triggers the red-to-yellow-to-green LED animation without sound.
A stable HIGH re-arms detection and wakes
the screen to Normal without playing a sound.

| Display state before docking | Display state after docking |
| --- | --- |
| Normal | Normal; idle timers unchanged |
| Dim | Dim; idle timers unchanged |
| Off / light sleep / deep sleep | Wake directly into Dim |

While the PC817C reports base power, device sleep and HA disconnection are
disabled, including when the screen is Off. Screen brightness and Dim/Off timing still follow the saved
schedule. Picking up from the dock always wakes the normal view; device power
saving then follows the undocked policy.

The screen shows a plug inside the battery icon when docked, including while
Dim. The web **Power** section shows a separate dock symbol beside the battery
and checks dock state every two seconds while the page is visible.
These checks read the debounced input and do not poll the charger or wake the
display. Both indicators mean base power and charging power are available. They do not
measure charge current or establish that the battery is full. The existing
`dockConnected()` and `dock_detected` names refer to this powered-base state.

When docking wakes a sleeping device, the active period's dim-to-off timeout
starts again. Deep sleep retains the last day/night period until a fresh HA
clock sample arrives. A held LOW from the PC817C does not repeatedly trigger feedback.
An ordinary boot while already docked shows the icon without replaying the animation.

`/diag` names the sensor with `dock_sensor=PC817C` and reports `dock_gpio_pin`,
raw `dock_gpio_level`, debounced `dock_detected`, `dock_powered`,
`dock_charge_available`, `dock_events`, `undock_events`, and `dock_leds_*`
animation counters. Power/availability values are `1` for LOW, `0` for HIGH,
and `-1` when GPIO sensing is unavailable. PMIC dock
inference remains separate. For a bench test, connect Port B **G to O**; leave
**V (5V)** alone. Disconnect USB and battery before modifying traces; grounding
an SCL contact still connected to the original bus would stop I2C communication.

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

Each day/night period has an **Off sleep mode** selection in web configuration:
**Light sleep** resumes the current view; **Deep sleep** stays asleep until a
keypad key is released, then boots into the main menu and reconnects Wi-Fi/HA.
Core A/B/C wake light sleep only. The wake press performs no entity action.
The active period's selection applies to scheduled Off, R5C1 Off and web Off.
Existing configurations default to Light sleep; saved timeouts are preserved.
Select Deep sleep for the desired period and use **Save and reboot** to apply it.

| Display state | Appearance | First device key press |
| --- | --- | --- |
| Normal | Selected theme and scheduled brightness | Performs its action and resets idle time |
| Dim | Black background and faint grey text; live values while docked, `--` while undocked | Wakes and performs the action; suspended targets are refreshed first |
| Off | Backlight off | Wakes the display only; release and press again to act |

These rules cover the Core buttons and keypad, plus serial navigation while the
CPU is awake. Use a physical key to wake from sleep. A Core
A/B/C press used to wake an off display is suppressed until release; holding B
cannot open a detail panel during that wake press. In dim, navigation works on
the first press. A short B press or keypad/serial action captures its original
entity and control, refreshes that entity asynchronously, and then acts once.
Moving focus or changing pages while the read is pending does not redirect the
action. A failed/unavailable refresh, another suspension, or a ten-second timeout
skips the action. Extra actions for the same target are ignored while the check
is pending. Holding B in dim opens/closes detail without also toggling on release.

While undocked, HA values and clock messages are suspended in dim/off. The local monotonic clock
continues advancing from its last HA sample, so idle deadlines and day/night
scheduling still run. Background scheduling may make the display darker or turn
it off; it does not wake or brighten it.
Web **Wake**, **Dim**, and **Off** use saved brightness settings without saving
or running an entity action. Manual Dim, including R5C1, counts the active day
or night Off timeout from entering Dim; `0` leaves it dim until input. Changing
periods applies that period's Off timeout. R5C1 in Dim immediately enters Off.
A wake press restores normal mode and refreshes HA; the press performs no
entity action. Docked Off stays reachable through the web. Undocked sleeping
Off requires a device key or a docking event. No saved timeouts are changed.

## Home Assistant time

The firmware subscribes to an HA `now()` template over the authenticated
WebSocket connection. HA supplies the UTC timestamp, current UTC offset, and
timezone name immediately after authentication and at each minute boundary.
Time updates are pushed by HA; there is no periodic REST time poll or NTP server.

- HA's configured timezone and current offset determine the day/night schedule,
  including daylight saving and timezone changes while connected
- A 64-bit monotonic clock advances time between updates and while disconnected
- While offline or suspended in undocked Dim/Off, the last known offset is retained;
  timezone/DST changes are learned after reconnection, when a fresh clock
  subscription is created
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
The entity list loads asynchronously after the configuration page opens and is
cached for 60 seconds. Loading it does not pause device navigation. Dim/off stop
new entity-list downloads; an already cached selector can still be served.

## Web configuration

- No saved Wi-Fi SSID: open AP `HA-Remote`, then `http://192.168.4.1`
- Normal operation: `http://<device-ip>/` or `http://ha-remote.local/`
- With saved Wi-Fi credentials, the device keeps retrying a lost connection

Fields:

- Theme: `Flipper` (default) or `Classic`
- Power: Low battery warning, on by default; double red flashes below 25% while undocked
- Fallback brightness, 1-100%, default 30%
- Day/night normal/dim brightness, dim/off timeouts, and live Wake/Dim/Off previews
- Dynamic page blocks: title + eight slots
- Each slot: entity ID and display name
- Climate slots only: optional AUX entity/name and allowed MODE values (COOL/HEAT/DRY/FAN/AUTO)
- Hostname used by DHCP and for `http://<hostname>.local/`, including web firmware
  updates; up to 31 letters, digits, and hyphens
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
| `GET /entities` | Cached entity selector HTML; `202` while loading, `503` if suspended with no cache |
| `POST /save` | Save settings and reboot; the web form reconnects automatically |
| `POST /display` | Preview the display with form field `mode=wake`, `dim`, or `off`; settings are not saved |
| `GET /power` | Current approximate battery percentage as text |
| `GET /power?format=icon` | Battery icon; an unavailable reading shows `?` |
| `GET /dock` | Physical dock state as JSON; no charger read or display wake |
| `GET /diag` | Menu, backlight, LED, and power diagnostics; reading it does not wake the display |
| `GET /screenshot.bmp` | Stream the current display as `ha-remote-screen.bmp`; does not redraw or wake the display |
| `GET /update` | Firmware upload form; requires the firmware username/password |
| `POST /update?size=<bytes>` | Authenticated firmware upload using multipart field `firmware` |

Backlight diagnostics include `backlight=normal|dim|off`, `backlight_pwm` (0–255),
`dim_theme` (0/1), and `display_override` (-1 for automatic, 1 for Dim preview,
2 for manual Off). Dim follows the active saved Off timeout. Sleep diagnostics
are retained across light sleep and available after waking.
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

HA HTTP requests run asynchronously. Navigation, display scheduling, web handling,
and WebSocket updates continue while a command or state read is in progress.
Completions apply on the main loop and preserve any newer live state. Up to eight
requests can be outstanding; commands take priority over new background reads.
Further commands for an entity already queued or running are ignored until it
completes, so repeated presses cannot calculate changes from stale values.
Commands are reconciled through a delayed state read even after an HTTP error,
since HA may have acted before the response failed.

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
| r5c1 (.) | Normal → Dim → Off (sleep) → wake only |
| 0 | Cancel a temperature edit; otherwise return to the main menu |
| r5c4 (=), short or long press | Open AC detail if available; otherwise toggle/run the selected entity. In AC detail, activate the control or confirm an edit |

Number positions are remapped on the physical keypad:

```text
       c1   c2   c3   c4
r2      1    2    3
r3      4    5    6    +
r4      7    8    9    -
```

There are eight menu slots, so 9 has no assigned slot. The physical bottom-left
`.` key (r5c1) dims from either the menu or AC detail. Pressing it in Dim puts
the device to sleep immediately; pressing it in Off only wakes the device.
Serial `.` still opens AC detail when awake.
R5C3 has no assigned action.

### Deep-sleep diagnostics

After waking, `/diag` reports `deep_sleep_entries`, `deep_sleep_keypad_wakes`,
`deep_sleep_timer_wakes`, `deep_sleep_dock_wakes`, `deep_sleep_undock_wakes`,
`deep_sleep_errors`, and `deep_sleep_last_wake` (`none`, `keypad`, `dock`,
`undock`, `timer`, or `other`). Counters survive deep sleep in RTC memory and
reset on other kinds of boot. `off_sleep_mode` shows the active period's saved
selection. Deep sleep has no periodic timer. The keypad wakes the
normal view; docking wakes Dim silently with LED feedback. Boot consumes the keypad wake
byte before normal input, including retries after a failed I2C read.

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
- WebSocket `ws://<host>:8123/api/websocket` with a `subscribe_trigger` state
  filter containing deduplicated configured primary and AUX IDs; attribute-only
  changes also update the remote
- WebSocket `render_template` subscription for [HA time](#home-assistant-time)
- Initial resync after WebSocket authentication and every 30 minutes
- REST resync fetches all configured entities and climate AUX entities; only
  visible rows redraw. All configured entities receive WebSocket updates
- HTTP requests, including the web entity selector, run on a worker while the
  main thread continues WebSocket processing. Actions still wait for their HTTP
  result; live state events received during a REST fetch take precedence over
  that snapshot
- State subscription failures use 30-second REST reconciliation and retry the
  subscription every 30 seconds. The `subscribe_trigger` operation requires an
  HA administrator token, as does the existing clock template subscription
- Entity-list JSON downloads have a 15-second total read deadline; a stalled or
  trickling HA server cannot hold web configuration indefinitely

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
The configured hostname (`ha-remote` by default) is applied before Wi-Fi starts,
so DHCP and mDNS use the same name. mDNS serves `http://<hostname>.local/`;
router-managed names such as `<hostname>.lan` depend on the router's DHCP/DNS
configuration. `/diag` reports `configured_hostname` and the actual station
interface's `wifi_hostname`. A router may retain the previous lease name until
its lease or DNS cache is refreshed.

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
python3 tools/test_dim_actions.py
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/ui_font_data.cpp tools/test_ui_font.cpp -o /tmp/m5-ui-font-test
/tmp/m5-ui-font-test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_ha_clock.cpp -o /tmp/m5-ha-clock-test
/tmp/m5-ha-clock-test
python3 tools/test_ha_clock_ws.py
python3 tools/test_ha_http_worker.py
python3 tools/test_web_config.py
python3 tools/test_network_hostname.py
python3 tools/test_connected_power.py
python3 tools/test_idle_io.py
python3 tools/test_off_sleep.py
python3 tools/test_deep_sleep.py
python3 tools/test_dock_wake.py
python3 tools/test_dock_led_task.py
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_low_battery_warning.cpp -o /tmp/m5-low-battery-test
/tmp/m5-low-battery-test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_dock_led_animation.cpp -o /tmp/m5-dock-led-test
/tmp/m5-dock-led-test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tools/test_dock_input.cpp -o /tmp/m5-dock-input-test
/tmp/m5-dock-input-test
python3 tools/test_sleep_config.py
```
