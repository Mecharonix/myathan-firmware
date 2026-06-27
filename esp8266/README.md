# MyAthan — ESP8266 firmware

This folder is the **ESP8266** build of MyAthan (the prayer-reminder device with a
JQ6500 audio module). It is self-contained and **completely independent** from the
ESP32-C3 PlatformIO project at the repository root. Nothing outside this `esp8266/`
folder is used by, or affected by, this firmware.

## Layout
- `MyAthan_OTA/MyAthan_OTA.ino` — the Arduino sketch (single file).
- `libraries/JQ6500_Serial/` — vendored dependency so CI builds reproducibly.

## Build locally
**Arduino IDE:** open `MyAthan_OTA/MyAthan_OTA.ino`, select board **NodeMCU 1.0 (ESP-12E)**, and upload.

**arduino-cli:**
```bash
arduino-cli core install esp8266:esp8266@3.1.2
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --libraries esp8266/libraries esp8266/MyAthan_OTA
```

## Over-the-air updates
`.github/workflows/esp8266.yml` compiles this sketch on every change to `esp8266/**`
on `main` and publishes `firmware.bin` + `version.txt` to the **`esp8266-latest`**
pre-release tag. The device checks that tag from its web UI
(`myathan.local` → **Device** → *Check for Updates*), compares `version.txt` against
its built-in `FW_VERSION`, and flashes itself if a newer build is available.

**To ship an update:** bump `FW_VERSION` in the sketch, merge to `main`, and the device
will offer the new version on its next check.

> The `esp8266-latest` tag is a **pre-release**, deliberately kept separate from the
> ESP32-C3 `v*` releases so the two devices never pull each other's binaries.
