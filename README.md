# esp32-monitoring

Firmware for a small physical dashboard: two OLED panels driven by an
ESP32-S3, polling a self-hosted "aggregator" service over HTTP for host and
Docker container stats, plus current weather from Open-Meteo. Built with
[PlatformIO](https://platformio.org/).

## What it does

The two 128x64 SSD1306 panels act as a master-detail pair. The left panel
cycles through "host", then each Docker container in turn, showing which
item is currently up (big name + page indicator); the right panel shows
that item's live numbers (CPU/MEM/DISK/temp for the host; CPU/MEM/status
for a container). Two independent timers drive this: one pulls fresh data
from the aggregator every 15s, the other advances the shown item every 5s,
decoupled so the display keeps cycling smoothly even between fetches.

After 10 minutes with no BOOT button press, the display switches to a
**screensaver**:

- Left panel: an NTP-synced, DST-aware clock with a bouncing DVD-logo icon.
- Right panel: a repeating cycle of current weather / today's forecast,
  interleaved with full-screen animations driven by live data --
  a system-pulse equalizer, CPU/MEM load bars with network throughput, a
  "fleet grid" one-cell-per-container overview, a CPU/MEM trend sparkline,
  plus two purely decorative ones (a starfield and Matrix-style rain).
- Every 40s, both panels take over for a 6s **Pong** interlude, treating
  the two panels as one wide canvas.
- If a container has gone down recently, both panels pre-empt everything
  else and blink a warning until it clears.

**BOOT button:**

| Action | In data mode | In screensaver |
|---|---|---|
| Short tap | Skip to next page | Wake up |
| Long hold (800ms) | Force an immediate data refresh | Toggle a manual dim override (e.g. for a meeting) |

Both panels also dim automatically overnight (22:00-07:00 by default), and
a task watchdog reboots the board if the main loop ever hangs (the
realistic failure mode being a stuck WiFi call).

## Hardware

- An ESP32-S3 board with native USB (tested target: `esp32-s3-devkitc-1`;
  built for something like an ESP32-S3-Zero).
- Two SSD1306 128x64 OLED panels, I2C, both at address `0x3C`, each on its
  **own** I2C bus (the S3 has two hardware I2C controllers, so both panels
  can share the same address):
  - Display 1 (left): SDA -> GPIO 8, SCL -> GPIO 9
  - Display 2 (right): SDA -> GPIO 6, SCL -> GPIO 7
- The BOOT button, wired active-low to GPIO 0 (this is the board's stock
  BOOT button on most ESP32-S3 dev boards -- no extra wiring needed).

Pins and timings live in `include/config.h` if your wiring differs.

## The aggregator

This firmware is a *client*: it expects some other service on your network
(not part of this repo) serving a JSON status blob over plain HTTP, with an
API key header. It polls this shape:

```
GET /status
X-API-Key: <your key>
```

```json
{
  "host": {
    "cpu_percent": 12.3,
    "mem_percent": 45.6,
    "mem_used_mb": 1024,
    "mem_total_mb": 8192,
    "disk_percent": 30.1,
    "temp_c": 52.0,
    "uptime": "3d 4h",
    "network_rx_bytes_per_sec": 12345.0,
    "network_tx_bytes_per_sec": 6789.0
  },
  "containers": [
    {
      "name": "traefik",
      "status": "running",
      "cpu_percent": 1.2,
      "mem_used_mb": 64,
      "mem_total_mb": 512,
      "down_recently": false,
      "down_duration_seconds": 0
    }
  ],
  "host_history": {
    "cpu_percent": [12.0, 14.5, 13.1, "..."],
    "mem_percent": [40.0, 41.2, 45.6, "..."]
  }
}
```

`temp_c`, `network_rx_bytes_per_sec`/`network_tx_bytes_per_sec`, and
`host_history` are all optional -- the firmware falls back to "n/a" or "not
enough trend data yet" if they're missing, rather than failing the fetch.

## Setup

1. Copy the secrets template and fill in your real values:

   ```sh
   cp include/secrets.h.example include/secrets.h
   ```

   `include/secrets.h` is gitignored -- it holds your WiFi password and
   aggregator API key, and should never be committed.

2. Adjust `include/secrets.h`:
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `DASHBOARD_URL` -- your aggregator's `/status` endpoint
   - `DASHBOARD_API_KEY` -- must match the aggregator's own config
   - `WEATHER_LAT` / `WEATHER_LON` -- coordinates for the Open-Meteo call

3. Build and flash:

   ```sh
   pio run -t upload
   ```

4. Watch the serial log (also useful for confirming WiFi/NTP/fetch status):

   ```sh
   pio device monitor
   ```

No certificate pinning is used for the Open-Meteo HTTPS call
(`WiFiClientSecure::setInsecure()`) -- a deliberate simplification since
it's read-only public weather data with no credentials involved. The
aggregator call itself is plain HTTP, so it's assumed to be running on your
local network.

## Project layout

`src/main.cpp` is just `setup()`/`loop()`; everything else is split by
concern:

| Module | Owns |
|---|---|
| `watchdog` | Task watchdog -- reboots on a hung main loop |
| `display_hw` | The two SSD1306 panels, I2C buses, boot splash |
| `draw_helpers` | Small reusable drawing/formatting primitives (icons, duration/rate formatting) |
| `wifi_manager` | WiFi connect/reconnect |
| `time_sync` | NTP sync, plus night-schedule + manual-override dimming |
| `weather` | Open-Meteo fetch |
| `dashboard` | Aggregator fetch + data-mode item/paging state |
| `dashboard_view` | Data-mode rendering (the master-detail pages) |
| `mode` | `DisplayMode` (data vs. screensaver) + idle-activity clock |
| `button` | BOOT button debounce, tap/hold dispatch |
| `animations` | The full-screen screensaver animations |
| `screensaver` | Screensaver orchestration: clock, weather cycle, downtime alert, Pong |

## Configuration knobs

Everything tunable lives in `include/config.h`: fetch/page intervals, idle
timeout before the screensaver kicks in, screensaver redraw rate, the
night-dimming window, and the timezone string (`TZ_STRING`, currently
`GMT0BST` for the UK with automatic BST). Per-animation constants (DVD
speed, star count, Pong speed, etc.) live next to the animation itself in
`screensaver.cpp` / `animations.cpp`.