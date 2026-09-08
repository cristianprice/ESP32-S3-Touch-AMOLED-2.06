# ESP32-S3 Touch AMOLED firmware instructions

## Tooling

This is an ESP-IDF 5.5.1 application built through PlatformIO. Use the only configured environment, `esp32-s3-touch-amoled-2-06`:

```sh
# Build
pio run -e esp32-s3-touch-amoled-2-06

# Flash the connected board
pio run -e esp32-s3-touch-amoled-2-06 --target upload

# Open the serial monitor (115200 baud)
pio device monitor -e esp32-s3-touch-amoled-2-06

# Build, stage, and upload the Angular static files, then the firmware
pio run -e esp32-s3-touch-amoled-2-06 --target uploadall

# Run all PlatformIO tests
pio test -e esp32-s3-touch-amoled-2-06

# Run one test suite directory, for example test/test_time_mgmt/
pio test -e esp32-s3-touch-amoled-2-06 -f test_time_mgmt
```

There are currently no test source files or configured lint command. Add PlatformIO tests below `test/<suite-name>/` so they can be selected with `-f <suite-name>`. Do not use `--verbose` for PlatformIO builds.

`platformio.ini` selects the PIOArduino Espressif32 platform, the `esp32-s3-devkitc1-n16r8` board, ESP-IDF, 16 MB QIO flash, and 8 MB octal PSRAM. `sdkconfig.defaults` and `partitions.csv` are hardware-critical: they enable octal PSRAM and Wi-Fi CSI, select the custom 8 MB factory application / 7 MB SPIFFS layout, and enable the LVGL fonts used by the UI.

`config-ui/` is an independent Angular 22 client. Its production output is staged in `data/` by the PlatformIO pre-build script, then stored in the firmware's `storage` SPIFFS partition by the `uploadfs` target. Run its commands from that directory:

```sh
# Start the development server
npm start

# Build the client
npm run build

# Run all Vitest unit tests once
npm test -- --watch=false

# Run one spec file once
npm test -- --include src/app/app.spec.ts --watch=false

# Check formatting
npx prettier --check .
```

The Angular client has no lint script configured. Its application is a standalone, routed Angular app under `config-ui/src/app/`; use the `app` selector prefix, SCSS component styles, and `public/` for static assets.

## Architecture

- The root `CMakeLists.txt` creates the ESP-IDF project. `src/` is its application component; its `CMakeLists.txt` explicitly enumerates every compiled source. Add new application `.c` files there and expose local headers through `INCLUDE_DIRS "."`.
- `config-ui/` is a separate Node/Angular workspace, not part of the firmware CMake build. Its Angular CLI configuration uses the application builder, development server, Vitest unit-test builder, and production output at `config-ui/dist/config-ui/`.
- `app_main()` in `src/main.c` starts the Waveshare BSP display, locks LVGL while creating initial UI, starts time management and the sleep timer, registers the touch handler, then starts the power-button task. The resettable timer enters deep sleep after 10 seconds without input.
- `src/time_mgmt.c` owns the PCF85063 RTC I2C device and the initial clock screen. It initializes the shared BSP I2C bus, validates or initializes RTC time from the build timestamp, and keeps LVGL clock labels updated with a one-second timer.
- The first touch transitions once to `src/main_menu.c`: it stops the clock timer, clears the active LVGL screen, then creates the battery toolbar and centered 3×3 icon grid. The third tile opens motion detection; the other tiles are placeholders.
- `src/motion_detection.c` pauses the sleep timer, configures station-mode promiscuous Wi-Fi CSI capture with power saving disabled, and sweeps channels 1–11. It passes CSI magnitude samples from the Wi-Fi callback to an LVGL timer through a FreeRTOS queue, then plots consecutive-sample change. Its back button stops CSI/Wi-Fi, deletes the queue and refresh timer, recreates the main menu, and resumes the sleep timer.
- `src/settings_ui.c` pauses the sleep timer, starts the open `WatchConfig` Wi-Fi access point, mounts the `storage` SPIFFS partition, and serves the Angular static files at `http://192.168.4.1`. Its back button stops HTTP, unmounts SPIFFS, stops Wi-Fi/networking, and releases NVS resources before returning to the menu.
- `src/power_button.c` owns the AXP2101 I2C device and a FreeRTOS polling task. A short power-button press resets the inactivity timer. `src/deep_sleep.c` saves the RTC, turns off the backlight, and configures GPIO0 as the active-low external wake source before sleep.
- `icons/` contains the source image assets. Files in `src/assets/` are compiled LVGL image descriptors used by firmware; for example, `abstract_timekeeper.c`, `chart_icon.c`, and `settings_icon.c` expose descriptors declared in their matching headers. Preserve the source artwork and its corresponding descriptor when adding or replacing an image.
- The Waveshare BSP, LVGL, display/touch, and codec dependencies are ESP-IDF managed components. Their requested versions are in `src/idf_component.yml`, while `dependencies.lock` pins the resolved dependency graph. Do not edit `managed_components/` for application behavior; update the manifest and regenerate the lockfile through the ESP-IDF/PlatformIO dependency workflow when dependencies must change.

## Repository conventions

- Use ESP-IDF error semantics: public module operations return `esp_err_t`, validate arguments and initialization state, and callers that cannot recover use `ESP_ERROR_CHECK`. Hardware-task errors are logged with a module-local `TAG` and `esp_err_to_name()`.
- Reuse the BSP-owned I2C bus (`bsp_i2c_init()`, `bsp_i2c_get_handle()`) rather than creating another bus. RTC and PMIC modules add their respective device handles to that shared bus.
- Create or mutate LVGL objects while the display is locked when operating outside the LVGL context. Preserve the display lock/unlock pairing established in `app_main()` around initial UI setup.
- Any new user-input path must call `sleep_timer_reset()` after the timer is started, rather than starting a separate inactivity delay. Full-screen activities that must remain awake use `sleep_timer_pause()` on entry and `sleep_timer_resume()` when returning to the menu.
- CSI callbacks run in the Wi-Fi task and must not access LVGL. Pass compact measurements through the motion-detection queue and update UI objects only from its LVGL timer.
- `tools/build_config_ui.py` runs before PlatformIO builds: it builds `config-ui/` and copies the production browser bundle into `data/`. Its `uploadall` target uploads the SPIFFS static files first, then uploads the firmware in a separate PlatformIO invocation.
- Keep module state private (`static`) and expose narrow APIs in the colocated headers. Each application source/header pair is registered directly from `src/CMakeLists.txt`.
- UI dimensions and colors are intentional fixed values for the board's 410x502 display. Keep the menu's 38 px toolbar/content split, centered 3×3 grid, 110×95 px buttons, 10 px column gap, and 20 px row gap consistent when changing its layout.
