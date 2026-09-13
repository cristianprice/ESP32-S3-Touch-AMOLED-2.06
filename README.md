# ESP32-S3 Touch AMOLED 2.06 Firmware

Feature firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06. It provides a
touch-first LVGL watch interface, Wi-Fi tools, SD-card file services, recording
and playback, wireless CSI motion visualization, activity tracking, RTC/NTP
time management, and MP3 Internet Radio.

The firmware is an ESP-IDF 5.5.1 application built with PlatformIO. Its
configuration portal is a separate Angular application that is built into the
SPIFFS filesystem image.

## Hardware target

| Component | Configuration |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-2.06 |
| MCU | ESP32-S3 |
| Display | 410 x 502 AMOLED touch display |
| Flash | 16 MB QIO |
| PSRAM | 8 MB octal PSRAM |
| Framework | ESP-IDF 5.5.1 |
| Build system | PlatformIO with the PIOArduino Espressif32 platform |
| UI | LVGL 9.5 |
| RTC | PCF85063 |
| PMIC | AXP2101 |
| Motion sensor | QMI8658 |
| Audio input | ES7210 microphone ADC |
| Audio output | ES8311 speaker codec |

The custom partition table assigns 8 MB to the factory application and 7 MB to
the `storage` SPIFFS partition.

## Features

### Clock and inactivity sleep

The device starts at the clock screen. Time is read from the PCF85063 RTC and
is refreshed once per second. If the RTC is invalid or uninitialized, the
firmware initializes it from the build timestamp.

The first touch opens the main menu. Input activity resets a one-shot
inactivity timer. The device enters deep sleep after 10 seconds without input.
Feature screens pause this timer while they are active and resume it when they
return to the main menu.

The physical power button puts the device into deep sleep. Deep sleep saves the
RTC state, turns off the backlight, and configures GPIO0 as the active-low
external wake source.

### Main menu and status toolbar

The main menu is a centered 3 by 3 icon grid with a battery and Wi-Fi toolbar.
The battery indicator reads the AXP2101 battery percentage. The Wi-Fi icon is
orange when the station interface has an assigned IP address and gray when no
usable station connection exists.

Menu tiles open:

1. FTP server
2. Settings portal
3. Motion detection
4. Steps meter
5. NTP client
6. Voice recorder
7. Multimedia player
8. Wi-Fi manager
9. Internet Radio

### Wi-Fi Manager

Wi-Fi Manager scans nearby access points and presents their SSID, security
state, and signal level. Selecting a protected network opens a password field
with an LVGL on-screen keyboard. Open networks can be connected without a
password.

Successful connections are saved to:

```text
/sdcard/wifi_credentials.json
```

The file stores the SSID and password as hexadecimal strings in JSON. This
encoding handles quote and backslash characters safely, but it is **not
encryption**. Treat the SD card as containing Wi-Fi credentials.

When Wi-Fi Manager opens, it scans first. If the saved SSID is visible, it
attempts automatic reconnection. A failed automatic connection opens the
password screen for that known SSID. A successful connection automatically
returns to the main menu.

The shared `wifi_network` module owns NVS, the network interface, ESP event
loop, Wi-Fi driver, station interface, AP interface, scans, and station
connection setup. Wi-Fi-dependent features reuse an existing valid station
connection rather than disconnecting it or starting another driver instance.

### Settings web portal

The Settings tile serves the Angular application stored in the `storage` SPIFFS
partition.

- With an active station connection, it serves the portal through the assigned
  station IP address.
- Without a station connection, it starts the open `WatchConfig` access point
  and serves the portal at `http://192.168.4.1`.

The portal exposes SD-card file management operations including browsing,
uploading, deleting files, and creating directories. It serves the Angular
production bundle and static assets with their appropriate MIME types.

Leaving Settings stops only the resources that Settings started. An existing
station connection is retained for other features.

### FTP server

The FTP tile starts a read-only FTP server rooted at:

```text
/sdcard
```

It supports directory listing, file-size lookup, and downloads using passive
FTP data connections. If the device is already connected to Wi-Fi, the server
uses that connection. Otherwise, it starts an open local access point for the
FTP session. The displayed IP address identifies the address clients should
use.

### Motion detection

Motion Detection visualizes changes in Wi-Fi Channel State Information (CSI).
It enables station-mode promiscuous reception, collects compact CSI magnitude
measurements in the Wi-Fi callback, passes them through a FreeRTOS queue, and
updates the LVGL chart from an LVGL timer. The chart displays change between
consecutive samples, making nearby movement visible as a changing trace.

When no station is connected, the feature sweeps Wi-Fi channels 1 through 11.
When an existing station connection is active, it reuses the association and
does not channel-hop, avoiding intentional disruption of that connection.

Back disables CSI/promiscuous mode, removes the callback, deletes the queue and
refresh timer, restores the appropriate Wi-Fi state, and returns to the main
menu.

### Steps Meter

The Steps Meter communicates directly with the QMI8658 motion sensor over the
shared BSP I2C bus. It provides:

- Step detection and a live step count
- Elapsed activity time
- Speed estimate in km/h
- Running-icon animation

The sensor is polled at 20 Hz with bounded I2C transactions to reduce the
chance that an unavailable device blocks the UI. The screen cleans up its LVGL
animations before navigation removes its objects.

### NTP Client

The NTP Client requires a usable station IP address. It tries the following
servers in sequence:

1. `pool.ntp.org`
2. `time.google.com`
3. `time.cloudflare.com`

After a successful query, it updates the ESP32 system clock and persists the
UTC timestamp to the PCF85063 RTC through the time-management module. It does
not start an access point or attempt to connect Wi-Fi by itself.

### Voice Recorder

Opening the recorder begins a WAV recording immediately. Audio is captured from
the ES7210 microphone at:

| Property | Value |
|---|---|
| Sample rate | 22,050 Hz |
| Format | PCM |
| Resolution | 16-bit |
| Channels | Mono |
| Microphone gain | 30 dB |

Files are written below:

```text
/sdcard/recordings
```

Recordings use timestamped names:

```text
recording-YYYY-MM-DD-HH-mm-SS.wav
```

The recorder creates a valid WAV header, tracks elapsed time, finalizes the
header when stopped, and displays errors when SD-card, codec, or write
operations fail.

### Multimedia Player

The Multimedia Player is an SD-card browser and WAV player. It navigates
folders and plays recorder-compatible WAV files:

- PCM
- 16-bit
- Mono
- 22,050 Hz

Playback controls include previous/next file selection, Play/Pause, progress,
elapsed time, Stop, and speaker volume. Audio output uses the ES8311 speaker
codec with an explicit output volume because the codec default is silent.

The library screen includes **Clear recordings**, which recursively deletes all
files and subdirectories inside `/sdcard/recordings` while preserving the
`recordings` directory itself.

### Internet Radio

Internet Radio plays the configured MP3 preset:

```text
Groove Salad
http://ice1.somafm.com/groovesalad-128-mp3
```

It requires an existing station Wi-Fi connection with a valid IP address. The
feature does not create an AP or initiate a Wi-Fi connection.

MP3 decoding uses the C-native Helix decoder from
[`chmorgan/esp-libhelix-mp3`](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3).
The stream reader, Helix decoder, and I2S speaker writer execute as separate
FreeRTOS tasks to maintain smooth playback:

1. The network task reads HTTP MP3 packets into a PSRAM-backed compressed-data
   pool.
2. The decoder task synchronizes Helix frames, retains incomplete input between
   packets, downmixes stereo when necessary, and resamples to 22.05 kHz mono.
3. The audio task writes queued PCM frames to the ES8311 I2S output.

Large network and PCM frame pools are stored in PSRAM. Task stacks remain in
internal RAM because Wi-Fi and HTTP code require internal task stacks for
reliable operation. Wi-Fi modem sleep is disabled during radio playback to
reduce receive latency and restored when playback stops.

The primary control supports Play/Pause. A rapid Stop/Start sequence is
deferred until the prior stream task completes cleanup, preventing duplicate
decoder-stack allocation.

> **License note:** the ESP-IDF component wrapper is Apache-2.0, but Helix
> decoder sources are distributed under the RealNetworks Public Source License
> (RPSL) or RealNetworks Community Source License (RCSL). Review those terms
> before redistributing the firmware.

## Project layout

```text
.
├── src/                 ESP-IDF application component
│   ├── assets/          Generated LVGL RGB565 image descriptors
│   ├── internet_radio.* MP3 HTTP radio and playback pipeline
│   ├── wifi_network.*   Shared Wi-Fi lifecycle and station/AP management
│   ├── wifi_manager.*   Scan, credentials, keyboard, and auto-reconnect UI
│   ├── sd_card.*        Shared SD card mounting support
│   ├── time_mgmt.*      PCF85063 RTC and clock screen
│   └── ...              Individual UI and hardware feature modules
├── icons/               Source SVG artwork for menu icons
├── config-ui/           Standalone Angular configuration portal
├── data/                Staged Angular production bundle for SPIFFS
├── tools/
│   └── build_config_ui.py  Builds and stages config-ui before firmware builds
├── partitions.csv       8 MB application / 7 MB SPIFFS layout
├── platformio.ini       PlatformIO environment configuration
└── sdkconfig.defaults   ESP-IDF hardware defaults
```

`src/CMakeLists.txt` explicitly lists every application source file. Add a new
application `.c` file there when creating a firmware module.

`icons/` contains source artwork. Matching files in `src/assets/` are generated
RGB565 descriptors compiled into firmware. Preserve the source SVG and
regenerate the descriptor when artwork changes.

## Build and flash

Install PlatformIO with ESP-IDF support, then use the only configured
environment:

```sh
# Build the Angular portal, stage it in data/, and build firmware.
pio run -e esp32-s3-touch-amoled-2-06

# Upload firmware to a connected board.
pio run -e esp32-s3-touch-amoled-2-06 --target upload

# Open the serial monitor at 115200 baud.
pio device monitor -e esp32-s3-touch-amoled-2-06

# Build/stage the portal, upload SPIFFS, then upload firmware.
pio run -e esp32-s3-touch-amoled-2-06 --target uploadall
```

The PlatformIO pre-build script runs the Angular production build and copies
the output to `data/`. `uploadall` is required after changing the configuration
portal because firmware upload alone does not update the SPIFFS files.

## Angular configuration UI

Run Angular commands from `config-ui/`:

```sh
cd config-ui

# Start the development server.
npm start

# Production build.
npm run build

# Run all Vitest tests once.
npm test -- --watch=false

# Run one test file once.
npm test -- --include src/app/app.spec.ts --watch=false

# Check formatting.
npx prettier --check .
```

The Angular app uses standalone routed components under `config-ui/src/app/`,
SCSS component styles, and `public/` for static assets.

## Firmware development notes

- The display/LVGL context runs on core 1. Do not mutate LVGL objects from Wi-Fi,
  HTTP, audio, or other background tasks.
- Defer screen changes from LVGL event callbacks with `lv_async_call()`. Directly
  cleaning the active screen inside the callback can delete the event target and
  cause instability.
- Use `sleep_timer_reset()` for new user-input paths. Full-screen activities
  should use `sleep_timer_pause()` on entry and `sleep_timer_resume()` on return.
- Reuse `bsp_i2c_init()` and `bsp_i2c_get_handle()` rather than creating another
  I2C bus.
- Wi-Fi callbacks must never call LVGL. Pass compact values to an LVGL timer
  through a FreeRTOS queue.
- Prefer PSRAM for large non-DMA data buffers. Keep Wi-Fi/network worker task
  stacks in internal RAM.
- `sdkconfig.defaults`, the generated SDK config, and `partitions.csv` contain
  hardware-critical PSRAM, CSI, filesystem, font, and partition settings.

## Testing

There are no firmware test sources yet. PlatformIO test commands are:

```sh
# Run all PlatformIO tests.
pio test -e esp32-s3-touch-amoled-2-06

# Run one test-suite directory, for example test/test_time_mgmt/.
pio test -e esp32-s3-touch-amoled-2-06 -f test_time_mgmt
```

Add firmware tests under `test/<suite-name>/` to make them selectable by
PlatformIO's `-f` option.

## Current limitations

- Internet Radio currently has one fixed HTTP MP3 preset. It does not yet expose
  station selection, HTTPS streams, ICY metadata/title updates, or reconnect
  controls.
- The media player supports only the WAV format emitted by the recorder.
- Wi-Fi credentials on the SD card are encoded for JSON safety but not encrypted.
- Motion Detection uses Wi-Fi CSI and can still be affected by local RF traffic,
  access-point behavior, and Wi-Fi driver constraints.
