# SkyGlass 🛩️

<p align="center">
  <a href="https://silentwolf75.github.io/skyglass/"><img src="https://img.shields.io/badge/Flash%20in%20browser-FF6D00?logo=googlechrome&logoColor=white" alt="Flash in browser"></a>
  <img src="https://img.shields.io/badge/boards-ESP32--S3%20%C2%B7%20ESP32--P4-E7352C?logo=espressif&logoColor=white" alt="Boards: ESP32-S3 and ESP32-P4">
  <a href="https://github.com/SilentWolf75/skyglass/releases"><img src="https://img.shields.io/github/v/tag/SilentWolf75/skyglass?label=firmware&color=7B42BC" alt="Firmware version"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/code-MIT-2088FF" alt="License: MIT"></a>
  <a href="https://github.com/SilentWolf75/skyglass/stargazers"><img src="https://img.shields.io/github/stars/SilentWolf75/skyglass?style=social" alt="GitHub stars"></a>
</p>

A live **ADS-B aircraft radar** for round touch displays — the **Waveshare
ESP32-S3-Touch-AMOLED-1.75** (466×466 AMOLED) and the **ESP32-P4-WIFI6-Touch-LCD-4C**
(720×720 IPS). It pulls nearby aircraft from a free online feed over
WiFi and plots them on a touch radar scope centred on your location: real callsigns, real
altitudes, aircraft drawn as the type they actually are, with photos and route lookups on tap.

It also does weather radar with a two-hour loop, marine traffic, a clock face, and updates
itself over WiFi. Point it at your own dump1090 receiver and it needs no internet at all.

<p align="center">
  <img src="docs/img/tour.gif" width="340" alt="SkyGlass cycling through its screens">
</p>
<p align="center"><sub>Every screen, captured off the 720×720 panel: traffic over Kansas City, the detail card, contact list, a tracked flight, weather radar, forecast, clock and About.</sub></p>

## Screens

Every image on this page is the **real framebuffer from my device**, pulled over WiFi with
`python tools/grab_screens.py` — not photographs and not a simulator.

| Radar | Detail card | Contacts |
|:---:|:---:|:---:|
| <img src="docs/img/screens/radar.png" width="240"> | <img src="docs/img/screens/detail.png" width="240"> | <img src="docs/img/screens/list.png" width="240"> |
| Aircraft drawn by type, altitude-coloured, airports labelled by ident with their real runways | Tap a contact: callsign, registration, type, altitude, speed, heading, squawk — plus airline, route and a photo of the actual airframe | Nearest-first contact list |

| Tracked flight | Stats | Clock |
|:---:|:---:|:---:|
| <img src="docs/img/screens/tracked.png" width="240"> | <img src="docs/img/screens/stats.png" width="240"> | <img src="docs/img/screens/clock.png" width="240"> |
| Follow one flight: progress along the great circle, distance to run and ETA | Traffic summary and how to reach the config page | Watch face with a seconds arc and current conditions |

| Precipitation | Satellite | Forecast |
|:---:|:---:|:---:|
| <img src="docs/img/screens/wx-radar.png" width="240"> | <img src="docs/img/screens/wx-cloud.png" width="240"> | <img src="docs/img/screens/forecast.png" width="240"> |
| RainViewer echoes with aviation-style overlays | EUMETSAT cloud-type imagery | Three days with vector weather icons |

| Boot splash | About |
|:---:|:---:|
| <img src="docs/img/splash.png" width="240"> | <img src="docs/img/screens/about.png" width="240"> |
| Painted per-pixel into a PSRAM canvas: gradient sky, stars, lit clouds, a live-looking scope and a banking airliner | The last screen: build date, board, chip, hostname, IP, uptime, feed and where the source lives |

### Themes

Long-press the screen to cycle, or pick one on the config page — **Phosphor**, **Orb**,
**Amber CRT**, **Military** and **Red CRT**, all five captured on the device:

<p align="center">
  <img src="docs/img/themes.gif" width="300" alt="The five themes cycling">
</p>

**Military** is night-vision rather than a green repaint: every contact is one phosphor
green and altitude reads as *brightness*, not hue. That makes the emergency ring the only
non-green thing on the scope, and the whole screen works without relying on colour vision.
**Red CRT** keeps dark adaptation, which is the one to leave running overnight. The scope's
own controls — the range button, the TRACK button — follow whichever theme is active.

## Features

### Traffic
- **Live ADS-B** from [airplanes.live](https://airplanes.live) (free, non-commercial), falling
  back to [adsb.lol](https://api.adsb.lol). Polled every couple of seconds through a
  memory-safe streaming parser with a hard aircraft cap. An empty sky is normal, so the
  fallback backs off rather than doubling the request rate against both feeds all night.
- **Aircraft icons by type**: the ICAO type code picks a silhouette — swept-wing jets in three
  sizes, straight-wing props, helicopters with a rotor disc, fighters, gliders — each rotated
  by track. Falls back to a generic glyph for unknown types, and can be switched off.
- **Detail card on tap**: callsign, registration, type, altitude, vertical speed, ground speed,
  distance, bearing, squawk, **airline name**, **origin → destination** from adsbdb, and a
  **photo of the actual airframe** from planespotters (cached in NVS). Airliners and bizjets are
  usually covered; light GA aircraft often aren't in the photo index.
- **Tracked flight**: press **TRACK** on any card to follow it — progress along the great
  circle, distance remaining and ETA from ground speed. A tracked contact is pinned so the
  on-screen cap never drops it as it flies away.
- **Military highlighting**: contacts flagged military in the feed get corner brackets.
- **Filters**: minimum altitude, hide ground traffic, military-only, and up to **120** aircraft
  on the scope.
- **Marine traffic** (optional, free [aisstream.io](https://aisstream.io) key): switch the scope
  from aircraft to AIS vessels — cyan hulls oriented by course with ship names; tap for MMSI,
  speed over ground, course, distance and bearing. Deliberately one or the other, not an
  overlay: ships and aircraft live at completely different scales.

### Maps & environment
- **Map background**: dark or light [CARTO](https://carto.com/attribution/) basemap tiles under
  the scope, resampled through the radar's own projection so roads and coastlines line up with
  the range rings at every zoom. Tiles download one at a time so the live feed never stalls, and
  a **visibility slider** (0–100%) fades the basemap live.
- **Weather**: RainViewer precipitation radar, EUMETSAT cloud imagery, and a three-day
  Open-Meteo forecast with vector icons. Range rings honour your distance unit.
- **Weather radar loop**: RainViewer publishes thirteen frames at ten-minute steps, and the
  device keeps as many as its PSRAM allows — the full **two hours** on the P4, one hour on the
  S3 — playing them as a time-lapse with the frame time on screen. Frames arrive one per poll
  so the live aircraft feed never stalls; the whole history is about 36 KB.

### Your own ADS-B receiver
- **Point it at a local dump1090 / readsb / tar1090** instead of the internet: a PiAware or
  ADSB Exchange feeder on your network works as-is. Your own antenna, sub-second data, and no
  internet dependency for traffic. One field on the config page.
- **Source selector**: *Auto* prefers the receiver and falls back to the public feeds when it
  is unreachable, *Local only* never touches the internet — an empty sky over your antenna
  stays empty rather than being quietly topped up — and *Internet only* ignores the receiver.
- **GPS** (optional): a Quectel LC76G over I2C, or any plain NMEA module on a UART, sets the
  centre point from a live fix.

### The device itself
- **Touch**: tap to inspect, **double-tap** to cycle range, **pinch** to zoom continuously,
  long-press to change theme. Swipe between **Radar / List / Stats / Weather / Tracked / Clock /
  About**. Ranges run from 1 mi to 100 km.
- **Alert sounds** (ES8311 speaker): a soft cue for a new aircraft in range, an urgent one for
  emergency or military. Pick **Chime, Sonar ping, Marimba, Aircraft warning** or **Beep** per
  cue independently — or **upload your own WAV** from the config page. It's converted on the
  device (stereo downmixed, 8–48 kHz resampled to 16 kHz, normalised) and stored in flash, so it
  survives reboots without rebuilding.
- **Quiet hours**: a window that dims, blanks, or forces the clock view; a touch wakes it for
  15 seconds.
- **Clock**: 12- or 24-hour, applied to the HUD, the watch face and weather timestamps. Backed by
  a **PCF85063 RTC** so the time is right before WiFi, re-synced from NTP once online.
- **Battery aware** (AXP2101): charge level in the HUD, low warning, and a slower poll rate on
  battery.
- **Smart brightness**: idle auto-dim, plus **face-down sleep** via the QMI8658 IMU — flip it
  over to blank the screen.
- **GPS auto-location** (optional **-G** board variant): the onboard LC76G sets the centre point
  itself, with a satellite status icon while acquiring.
- **Self-update over WiFi**: checks this repo's GitHub Pages build for a newer version and
  installs it — no cable, no toolchain. On demand from the config page, or leave the 6-hourly
  auto-check on. Writes to the inactive OTA slot, so a failed download can't brick a working
  install.

## Hardware

Two boards, one firmware. Pick the environment that matches yours; everything above works
on both.

**[ESP32-S3-Touch-AMOLED-1.75](https://www.amazon.com/dp/B0F886SYQ6)** (ASIN `B0F886SYQ6`)
— the original target. ESP32-S3R8 (8 MB PSRAM, 16 MB flash), **CO5300** AMOLED 466×466 over
QSPI, **CST9217** touch, **QMI8658** IMU, **PCF85063** RTC, **AXP2101** PMIC, **ES8311**
audio + speaker, microSD. The IMU gives it face-down sleep and shake-to-refresh; the PMIC
gives it a battery readout.

**[ESP32-P4-WIFI6-Touch-LCD-4C](https://www.amazon.com/dp/B0FB9CQVRR)** (ASIN `B0FB9CQVRR`)
— bigger and sharper. ESP32-P4 RISC-V (32 MB PSRAM, 32 MB flash), 4" IPS **720×720** over
**MIPI-DSI**, **GT9271** touch, **ES8311** audio, microSD. The P4 has no radio of its own:
Wi-Fi 6 comes from an onboard ESP32-C6 over SDIO, which the firmware uses through the normal
`WiFi` API. No IMU, RTC or PMIC on this board, so those features degrade honestly rather than
reporting made-up values. The scope is drawn at 720×720 and runs about 12 fps — smooth, but
not faster than the smaller board; see [`docs/PORT_P4.md`](docs/PORT_P4.md) for why.

Per-board pins and geometry live in [`src/boards/`](src/boards), taken from the vendor
definitions rather than guessed; [`src/config.h`](src/config.h) keeps the app-level settings.

Each board answers to its own name on the network — `skyglass-s3.local` and
`skyglass-p4.local` — so you can run both at once without them fighting over one address.

## Flash from your browser (no toolchain)

1. Open the **[web flasher](https://silentwolf75.github.io/skyglass/)** in Chrome or Edge.
2. Plug the board in with a USB-C **data** cable and click **Install**.

> Works for **both boards** — the installer detects the chip and picks the matching
> build. To update a device that is already running, its own **Firmware update** page takes
> `SkyGlass-esp32p4-firmware.bin` (or `firmware.bin` for the S3) from the same site over WiFi.

> Leave **Erase device** unticked to keep your WiFi credentials, settings and uploaded alert
> sound. Tick it for a clean install or to recover a confused device.

The flasher writes bootloader, partition table, `boot_app0` and application to their own
offsets. It deliberately does *not* write one merged image starting at `0x0` — that image's
`0xFF` padding covers the NVS region at `0x9000`, which silently wiped settings on every web
flash regardless of the erase checkbox.

## Build & flash (PlatformIO)

```bash
pio run -e esp32-s3-amoled-175 -t upload     # 1.75" AMOLED board
pio run -e esp32-p4-lcd-4c     -t upload     # 4" P4 board
```

Serial log:

```bash
pio device monitor -b 115200
```

On first flash you may need to hold **BOOT** then tap **RESET**. On first boot, connect a phone
to the **`SkyGlass-Setup`** WiFi and enter your home network — aircraft appear within
seconds.

## Update over WiFi (no cable)

Easiest is the config page: **Check for update**, then **Install**. To push a local build
instead:

```bash
curl -X POST -F "f=@.pio/build/esp32-s3-amoled-175/firmware.bin" http://skyglass.local/update
```

Or upload `firmware.bin` by hand at `http://skyglass.local/update`. Use the **app-only**
image here, never the merged one.

## Configuration

Browse to `http://skyglass-s3.local/` or `http://skyglass-p4.local/` (or the device IP) on the
same WiFi — each board answers to its own name, so both can run at once. Centre point with a
map picker, display range, theme, time zone, brightness, map background and visibility, aircraft
filters, your local ADS-B receiver and which source to prefer, marine AIS and its key, quiet
hours, sounds, firmware update and WiFi reset. Settings persist in NVS.

### Diagnostics

`/diag` returns a health snapshot as JSON — free heap, minimum heap
since boot, largest contiguous internal block, free PSRAM, aircraft counts, and why the last
photo fetch succeeded or failed. `lv_free` / `lv_pct` / `lv_biggest` / `lv_frag` cover the
fixed LVGL object pool: exhausting it used to freeze the UI core outright, so the largest
free block and the fragmentation are the numbers to read when the display stops responding.
`lbl_*` time the floating-label layout and count how many labels it repositions per pass
(non-zero at rest means placements are churning). `sd_*` cover the microSD flight log:
card state, airframes on file, and the lookup hit/append ratio. It exists so those numbers can be read over WiFi instead of
only from a serial cable:

```json
{"fw":"1.17.0","uptime_s":438,"heap":233860,"heap_min":167168,"heap_largest":131060,
 "psram":21243772,"aircraft":19,"max_on_screen":120,"feed_cap":120,
 "lv_free":83308,"lv_pct":28,"lv_biggest":82828,"lv_frag":1,
 "lbl_us":8,"lbl_moves":0,"lbl_seen":0,
 "sd":"4-bit 29476 MB","sd_recs":1388,"sd_hit":1990,"sd_app":1388,"sd_rderr":0,
 "photo":"","fps":82.6,"draw_us":4498,"step_avg":2.99,"step_max":2.99,
 "frame_ms":76,"lvgl_ms":71.9,"rest_ms":6.9}
```

## Screenshots

`tools/grab_screens.py` walks every view and writes one PNG per screen, masking the corners
transparent to match the round panel. Pure stdlib — no Pillow, no ffmpeg:

```bash
python tools/grab_screens.py skyglass.local docs/img/screens
```

It drives the firmware's own `/view` and `/shot.bmp` endpoints, so it can select a contact,
track a flight and switch screens without anyone standing at the device.

The animated GIFs on this page are assembled from those stills by `tools/make_gif.py`
(the one tool here that needs Pillow). There is no sweep animation for a good reason:
`/shot.bmp` takes about 2.3 s per frame over WiFi against a ~5 s sweep, which samples
roughly two frames per rotation — a stutter rather than motion. Cycling whole screens is
honest at that capture rate and shows more anyway.

## Desktop simulator

The UI is portable LVGL and runs on a computer via SDL2 — useful for iterating without hardware:

```bash
pio run -e native -t exec
```

Mouse = touch · `T` = switch theme · close the window to quit.

### Screenshot regression net

Every push renders all thirteen screens in that simulator on CI and diffs them
pixel-for-pixel against the references in `tests/screens/`. Layout bugs are the ones this
project actually suffers from — text landing on text, rings drifting off their image,
labels clipped by the round bezel — and until now every one of them was found by flashing
a board and looking at it.

```bash
python tools/check_screens.py --shots shots            # compare
python tools/check_screens.py --shots shots --update   # accept as the new references
```

When a screen changes on purpose, download the `screens` artifact from the failed run,
look at the `-actual.png` images, and commit them as the new references. When it changes
by accident, the run tells you which screen and by how much.

The capture parks the sweep and the home-marker pulse (`radar::setStillMode`) so the
images are reproducible, and masks the clock's time/date band and the About screen's
build date, which move on their own.

## Repo layout

```
src/
  config.h           pins + tunables
  main.cpp           tasks, WiFi/NTP, web config + diagnostics, brightness/IMU glue
  display.*          CO5300 (Arduino_GFX) + LVGL bring-up
  radar_view.*       the radar scope, aircraft, themes
  aircraft_types.*   ICAO type designator -> drawing silhouette
  ui.*               views (radar/list/stats/weather/tracked/clock/about), cards, HUD
  touch_cst9217.*    capacitive touch (single touch + 2-point pinch)
  adsb_client.*      airplanes.live / adsb.lol fetch + parse
  photo.* photo_client.*        airframe photos (planespotters)
  route*.* route.*              origin -> destination lookup (adsbdb)
  airline.*                     operator name (offline table)
  map_bg.* map_client.*         CARTO basemap: tile fetch, stitch, reproject
  vessel.* ais_client.*         AIS marine traffic (aisstream.io WebSocket)
  coastline.*        vector coastlines and borders under the scope
  updater.*          self-update from GitHub Pages
  imu_qmi8658.*      accelerometer (face-down sleep)
  battery.*          AXP2101 battery gauge
  rtc_pcf85063.*     PCF85063 real-time clock
  sim_main.cpp       native SDL simulator (not flashed)
include/lv_conf.h    LVGL config (v8)
web/flash/           browser web-flasher (ESP Web Tools)
tools/               grab_screens.py, gen_alert_sound.py, gen_airports.py, gen_runways.py
scripts/             build_webflasher.sh
docs/                hardware / data-source / architecture / feature notes
```

## Credits

This firmware is a fork of **[socquique/capsule-radar](https://github.com/socquique/capsule-radar)**
by Quique Tortosa, which is the original project and the source of the core scope, themes and
device bring-up. It remains MIT licensed — see [`LICENSE`](LICENSE), which keeps his copyright
notice. Several features here were adapted from
**[yashmulgaonkar/FlightScnr_Pi](https://github.com/yashmulgaonkar/FlightScnr_Pi)**.

Related work worth knowing about:

- **[SkyGlass for the Waveshare ESP32-S3-Touch-LCD-2.1](https://github.com/alexzogh/skyglass/tree/port/esp32-s3-lcd-21)**
  by **@alexzogh** — a port to the 2.1" round LCD (ST7701).
- A 3D-printed enclosure for this board is published by the original author on
  **[MakerWorld](https://makerworld.com/en/models/2907695-skyglass-live-flight-radar-desk-gadget)**.

## Data & license

**Firmware / code: [MIT](LICENSE)** — fork and build on it freely, keeping the notice.

Aircraft data: **airplanes.live** and **adsb.lol** (free, **non-commercial / educational** — be
polite with request cadence). Routes: **adsbdb.com**. Airframe photos: **planespotters.net**,
proxied through **images.weserv.nl** for baseline-JPEG re-encoding; each photo is credited to its
photographer on the card and remains the property of its owner. Basemap tiles: **CARTO** /
**OpenStreetMap** contributors. Weather: **Open-Meteo**, **RainViewer**, **EUMETSAT**. Marine
AIS: **aisstream.io** (your own key).

Personal, hobby, non-commercial project.
