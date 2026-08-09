# Features — SkyGlass

Visual target: `assets/plane_radar_2.0_mockup.html`.

## Look (the "prettier")
- True-black AMOLED background; phosphor-green scope, glow on the sweep edge.
- Concentric range rings + crosshair + N/E/S/W rose; outer-ring range label (e.g. "15 km").
- Rotating sweep with a trailing alpha gradient.
- Aircraft as **plane glyphs rotated by track/heading** (not dots).
- **Altitude color map**: ≤3k ft red → 3–10k amber → 10–20k lime → 20–30k green → 30k+ cyan.
- **Fading trail** behind each aircraft (last N positions / direction tail).
- Center "you" dot with a soft pulse.
- Top HUD: WiFi strength, aircraft count, clock.

## Functions (the "more")
1. **Touch to inspect** — tap nearest glyph → detail card: callsign, type, registration (if available), altitude, ground speed, vertical-rate arrow, distance, bearing, squawk.
2. **Views** (swipe): Radar · List (sorted by distance) · Stats (count, closest, max altitude and range) · Weather. Tap an aircraft to open its detail card.
3. **Range zoom** — cycle 10 / 20 / 30 / 50 / 100 km and re-query the aircraft feed accordingly.
4. **Orientation** — north-up ↔ track-up toggle.
5. **Alerts** — highlight + speaker **ping** for: emergency squawks (7500/7600/7700), military (`dbFlags`), or a user watch-list of types (A380, B52…). Card flashes red (see RESCUE51 in the mockup).
6. **Night auto-dim** — use PCF85063 RTC to lower brightness after dusk.
7. **IMU gestures** (QMI8658) — face-down → screen sleep; face-up → wake; shake → force refresh.
8. **Setup & maintenance** — first-boot **captive portal** (WiFi creds + home lat/lon + range); settings in NVS; **OTA** updates.
9. **Three-day weather forecast** — a round-display layout with current temperature and condition, apparent temperature, humidity and wind, followed by three aligned daily columns for condition, high/low temperature and rain probability. Data comes from [Open-Meteo](https://open-meteo.com/), uses the saved or GPS-derived radar centre, follows the selected unit preset and refreshes every 30 minutes.
10. **WX precipitation radar** — a north-up, nearly fullscreen circular precipitation view centred on the saved/GPS position. Aviation-style range rings, centre marker, nearest-IATA context, current-weather footer and source timestamp are rendered as overlays. The approximately 75 km-radius image is cached in PSRAM and refreshed every five minutes from [RainViewer](https://www.rainviewer.com/api.html) (personal/educational use; availability is not guaranteed). This layer shows rain, snow and hail echoes—not ordinary cloud cover.
11. **Meteosat cloud imagery** — a location-centred, approximately 400 km-wide satellite view from the official [EUMETSAT EUMETView WMS](https://user.eumetsat.int/data-access/eumetview). The Cloud Type RGB product makes cloud structure and classification visible beneath the same aviation overlays, with a separate satellite timestamp and EUMETSAT attribution. Images require no API key, are decoded directly into PSRAM and refresh every ten minutes. Cloud colours describe satellite-derived cloud properties; they do not directly indicate precipitation severity.
12. **Weather-mode control** — the on-screen control cycles **WX Radar → Sat Clouds → 3-Day Forecast**. The imagery itself can also be tapped to advance to the next mode.
13. **Graceful network handling** — weather products load independently after WiFi connects, retain their last valid image during transient failures and retry failed requests without blocking the display. Status text distinguishes forecast, precipitation-radar and satellite-data waits.
14. **Desktop simulator** — the same 466×466 LVGL interface runs locally through SDL2. It includes mock aircraft, forecast data, precipitation cells and Meteosat-style cloud bands, allowing all three weather modes and round-screen layouts to be reviewed without flashing hardware. Live network imagery is fetched only by the ESP32 firmware.

## Weather data at a glance

| Mode | What it shows | Coverage | Provider | Refresh |
|---|---|---:|---|---:|
| WX Radar | Measured precipitation echoes | ~75 km radius | RainViewer | 5 min |
| Sat Clouds | Satellite-derived cloud types | ~400 km across | EUMETSAT | 10 min |
| 3-Day Forecast | Current conditions and daily outlook | Configured/GPS point | Open-Meteo | 30 min |

## Ported from FlightScnr_Pi
These bring the round-display Raspberry Pi tracker's ideas to the ESP32. Layers that
need a third-party account are off until a key is entered on the config page.

15. **Pinch zoom** — two-finger pinch on the scope scales the display range continuously
    (5–150 km) instead of stepping through fixed values. The gesture only re-projects the
    view; the feed re-query and the NVS write happen once, when the fingers lift. A pinch
    never registers as a tap or a swipe.
16. **Map background** — dark or light [CARTO](https://carto.com/attribution/) basemap
    tiles beneath the scope. The scope is azimuthal-equidistant and tiles are Web
    Mercator, so the mosaic is resampled per pixel through the inverse scope projection,
    keeping the map aligned with the range rings at any zoom. Tiles are fetched one per
    network-task iteration so a 9–16 tile build never stalls the aircraft feed; the old
    image hides as soon as the scope changes and reappears when the rebuild lands.
17. **Tracked flight** — TRACK on a detail card pins one contact. The Tracked view shows
    the route, a progress bar (flown / (flown + remaining), so a diversion still reads
    sensibly), distance to run and an ETA from ground speed. Endpoint coordinates come
    from the same adsbdb lookup that supplies the route labels and are cached with it.
    A pinned contact is exempt from the on-screen aircraft cap.
18. **Marine AIS** — a persistent [aisstream.io](https://aisstream.io) WebSocket
    subscribed to a box around the scope. Vessels draw as cyan hulls oriented by course
    with their names; a tap opens a vessel card (MMSI, SOG, COG, distance, bearing).
    Contacts expire after 15 minutes without an update. Needs a free API key.
    The scope plots **either** aircraft **or** vessels, chosen in settings — they share
    no scale, altitude or speed frame, so overlaying them reads as noise. The HUD count,
    the list view and tap targets all follow the selected mode.
19. **Local ADS-B source** — point the feed at a dump1090 / readsb / tar1090 receiver on
    your own network instead of the public APIs. The parser already accepted both the `ac`
    and `aircraft` array keys, so a PiAware or ADSB Exchange feeder works unmodified. A
    source selector chooses auto (receiver first, internet as fallback), local only, or
    internet only.
20. **Clock view** — a full watch face (time, seconds, weekday and date) with current
    conditions and a three-day strip. Refreshes on its own 1 s timer, only while visible.
21. **Quiet hours** — a configurable window (wrapping midnight) that dims, blanks, or
    forces the clock view. A touch restores normal brightness for 15 seconds.
22. **Airline identity** — the callsign's ICAO prefix resolves to an operator through an
    embedded table, so the name shows offline; the logo is downloaded on demand and
    flattened onto the card colour by the same image proxy the aircraft photos use.
23. **Type-based aircraft icons** — the feed's ICAO type designator selects a silhouette
    (`aircraft_types.*`): swept darts in three sizes for small/narrow/wide jets, a
    straight wing bar for turboprops and light aircraft, a long thin wing for gliders,
    a delta for fighters, and a rotor disc for helicopters. Shapes are vector polygons
    rotated by track, not bitmaps, so they cost no flash and stay sharp at any angle.
    The prefix table is ordered specific-to-general because several families collide
    (C208 is a turboprop, not a Cessna single; MD5x is a helicopter but MD8x is an
    airliner; B212 is a Bell helicopter, not a B-2). Unknown types fall back to the
    narrowbody dart, and the whole feature has an on/off switch.
24. **12/24-hour clock** — one shared formatter (`ui_format_clock`) drives the HUD, the
    clock face and the radar/satellite timestamps, so they can never disagree.
25. **Alert sounds** — five synthesised packs (chime, sonar, marimba, cockpit warning,
    plain beep) plus a user-uploaded sample. Notes carry a pitch glide, harmonic mix and
    exponential decay; loudness is equalised across packs by dividing each note by the
    true peak of its composite waveform (`wave_peak`) rather than by the sum of its
    harmonic amplitudes, which had made the richer packs up to half as loud.
    **New contacts and emergencies each pick their own sound**, so a quiet marimba can
    announce routine traffic while something urgent is reserved for emergencies. The
    alert mode is a bitmask (bit 0 = new, bit 1 = emergencies), which gives Off / new
    only / emergencies only / both, and lets the page hide the sound picker for whichever
    cue is disabled. Old installs are migrated under a new NVS key, since the previous
    value 2 meant "both" and would read as "emergencies only" as a bitmask.
26. **WAV upload** (`wav_upload.*`) — the config page accepts a WAV and converts it on
    the device: an incremental RIFF parser handles chunk boundaries falling anywhere in
    the stream, stereo is downmixed, any 8-48 kHz rate is linearly resampled to the
    codec's 16 kHz, and a second pass normalises to 89% full scale with a 10 ms tail
    fade. The result lands in LittleFS (the 3.5 MB `spiffs` partition) and is reloaded
    into PSRAM at boot, so a custom sound survives reboots and needs no rebuild.
    Rejections name the actual problem ("need 16-bit WAV", "compressed WAV",
    "sample rate must be 8-48 kHz") rather than failing generically.

## Runway outlines
Each airport also draws its actual runways, from the OurAirports thresholds
(`tools/gen_runways.py` -> `src/runways_data.h`): 14,847 runways, ~232 KB. Both ends are
projected independently rather than deriving one from a heading and a length, so a strip
lies on its real bearing at its real length - KIXD reads as its 04/22 and 18/36 rather
than a dot.

Deliberately not keyed to `airports_data.h`. Linking the two by index would mean
regenerating both from one snapshot every time, and they already disagreed by one airport
because the upstream data moved between generations; absolute endpoints cannot drift out
of alignment. Runways follow the airports toggle, draw under the markers so the ident
stays readable, and are dropped during projection once their projected length falls under
5 px - zoomed out they are smudges around a dot that is already there.

## Flight log (microSD, P4 only)
One fixed 32-byte record per airframe on the card, keyed by ICAO hex: visit count, first
and last seen, closest-ever approach. The detail card folds it into the route line as
`seen 7x, closest 1.2 nm`. Aggregates rather than a raw sighting list, because the
question being asked is "how often has this one been over", and answering that from an
append-only log would mean scanning it every time.

Three things this cost, all of them board-specific:
- **The SD rail is an on-chip LDO (channel 4), off at reset.** Without switching it on the
  card has no signal voltage and never answers - the mount fails with `ESP_ERR_TIMEOUT`
  and looks for all the world like an empty slot. Same shape as the DSI PHY on LDO 3.
- **`esp_vfs_fat_sdmmc_mount()` cannot be used here.** The ESP32-C6 radio is an SDIO
  device on the *other slot of the same SDMMC host*, and that helper always initialises
  the host and tears it down on failure. Using it froze the board with the slot empty.
  The mount is done by hand instead: tolerate a host that is already up, bring up slot 0
  only, register the FATFS drive directly, and never call `sdmmc_host_deinit()`.
- **Only real ICAO addresses are logged.** A local receiver also reports TIS-B and ADS-R
  tracks with ephemeral non-ICAO ids, which would otherwise arrive as thousands of new
  "airframes" a minute.

Known defect: roughly a third of contacts miss on lookup each poll and are appended
again, so the file grows faster than it should and visit counts read high. `sd_hit`,
`sd_app` and `sd_rderr` in `/diag` measure it. Nothing is destroyed; the settings page
can erase the log.

## Airport markers
The embedded list (`tools/gen_airports.py` -> `src/airports_data.h`) keeps every airport
carrying an IATA code, plus all large airports: 8,802 entries, ~129 KB.

Two details are easy to get wrong and were:
- **Small airports must be included.** OurAirports classes plenty of real, busy GA fields
  as `small_airport` - New Century AirCenter (KIXD) among them - so filtering by type
  alone silently drops the airport nearest a lot of users. Requiring an IATA code is what
  keeps the file small while admitting these; it excludes ~38,000 unnamed strips,
  heliports and closed sites.
- **Label with the identifier people actually use.** Airline airports are known by IATA
  (MCI, not KMCI); GA fields are known by their ICAO/FAA ident (KIXD - its IATA code,
  JCI, is one almost nobody would recognise). The generator picks per airport class.

Positions are `int32` degrees x 10,000 (~11 m). The previous `int16` x 100 encoding
quantised to ~1.1 km, invisible at a 100 km range but badly wrong for a field 3 km away
on a zoomed-in scope. Labels are drawn for every airport when 14 or fewer are on screen
and for large ones only beyond that, so a zoomed-in view names the local field without
turning a wide view into a wall of text.

## Self-update
The device updates itself over WiFi from the project's own GitHub Pages build
(`updater.*`). The web-flasher workflow already stamps `manifest.json` with the version
it compiled and publishes `firmware.bin` beside it, so nothing extra needs hosting: the
device polls the manifest, compares versions, and streams the image into the spare OTA
slot. The settings page shows the running version, the status, and an Install button
that appears only when the server is genuinely ahead.

Three details matter:
- **Automatic installing is off by default.** Automatic *checking* is on. The asymmetry
  is deliberate: a watchdog-crashing build reached that exact Pages site during
  development, and a fleet set to auto-install would have rebooted itself into a loop
  with no cable in reach.
- **Safety is structural, not careful coding.** `Update` writes to whichever of
  `ota_0`/`ota_1` is not running and only flips `otadata` once the image verifies, so an
  interrupted download or a power cut leaves the current firmware bootable.
- **The image is streamed in 1 KB chunks.** At >3 MB it must never be buffered whole,
  and the transfer aborts if the server stalls for 20 s.

Versions compare as integers (`1.8.4` -> `10804`), and anything unparseable sorts
lowest, so a malformed manifest can never be mistaken for an upgrade.

## Map basemap: why compose() is banded
Turning the basemap on used to put the device into a reboot loop within ~30 seconds.
`compose()` resamples every one of 466x466 scope pixels through an inverse
great-circle + Mercator projection - `sqrt`, `asin`, `atan2`, `sin`, `cos`, `log` per
pixel - and the ESP32-S3 has no double-precision FPU, so all of that is emulated in
software. Running it to completion in one call held CPU 0 for several seconds, IDLE0
never ran, and the task watchdog aborted.

It now composes `MAP_COMPOSE_ROWS` rows per `map_client_step()` call. The network task
already invokes that once per iteration and then sleeps 250 ms, so the work is spread
over ~20 calls and the scheduler gets the CPU back between bands. The visible cost is
that the basemap takes a few seconds to appear; the alternative was a device that
rebooted whenever the feature was enabled.

This is the failure mode to expect from anything heavy added to `adsb_task`: it shares
a core with IDLE0 and the watchdog, so long uninterrupted work there is fatal rather
than merely slow.

## Military contacts
The feed's `dbFlags` bit 0 marks military airframes. It reaches the audio alerts and the
feed filter, and now the display too: four violet corner brackets around the glyph, a
violet callsign in the list, and a `MIL` tag on the detail card. Violet because the
altitude ramp already owns red/amber/lime/green/cyan and the emergency halo owns red;
brackets rather than a ring so the two can never be confused at a glance.

Military traffic is rare enough that the marker would otherwise go unchecked until one
happened to fly past, so `/view?mil=1` draws every contact as military for inspection.
`/view?icon=N` does the same for the weather glyph set (a Kansas summer will not supply
snow on demand) - it has to latch, since `build_weather()` repaints from live data every
poll and would otherwise undo the override within two seconds.

## Render performance (and one instructive failure)
The sweep is the most expensive element on screen: every frame redraws a fan of wide
anti-aliased alpha lines *and* forces every layer beneath it (basemap, flow canvas,
chrome, coastline, airports) to recomposite over the wedge's bounding box. Measured on
hardware with 12-15 aircraft in range: ~9 fps with the sweep on, ~35 fps with it off.

Two changes are kept. Both are invisible — they change what is skipped, never what is
drawn — and together they buy a few fps for free:
- **Clip rejection** in the aircraft layer, so contacts outside the region being
  repainted are skipped. Orb's blip/arrow budget is claimed *before* the clip test, or
  a different set of aircraft would qualify per region and flicker.
- **The flow canvas is hidden when trails are off.** It is a full-screen alpha layer, so
  LVGL was reading and blending every pixel of it even when fully transparent — which is
  why turning trails off previously did not help. Trails off is now worth ~4 fps.

**Two changes were tried and reverted, and the reason matters.** Shortening the sweep
trail (38 deg -> 16 deg) and driving the sweep angle from the clock instead of a fixed
step per callback raised the measured frame rate from ~9 to a stable 15-16 fps — and
looked *worse* on the panel. Two reasons: the long fading trail visually masks angular
stepping, so shortening it exposed the judder it was hiding; and a time-based angle
converts variable frame times into variable step sizes, which reads as more judder than
uniform small steps even though the average speed is correct.

The lesson for anyone tuning this: frame rate is a proxy, not the goal. These particular
numbers were chosen by eye and should be changed by eye.

## Nice-to-have / later
- Sound themes / mute.
- Multiple saved home locations.
- microSD logging of seen aircraft.
- Local dump1090/readsb as an alternative feed source (FlightScnr_Pi supports this;
  useful if you run your own receiver).

## MakerWorld packaging
- Parametric printable enclosure for the round board (bezel + stand), à la the original radar.
- Publish firmware + STLs; include a looping GIF of the live sweep. The original radar earned MakerWorld "featured/boost" badges — same formula here for points toward the P2S.
