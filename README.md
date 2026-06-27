# MyAthan — ESP8266 firmware (branch `esp8266-ota`)

**This branch contains only the ESP8266 build of MyAthan and its over-the-air (OTA) update pipeline.**

It is deliberately kept separate from `main`, which holds the older **ESP32-C3** PlatformIO
project. That ESP32-C3 code is **not** on this branch and is left completely untouched on `main`.

> ⚠️ **Do not merge this branch into `main`.** Doing so would delete the ESP32-C3 project from
> `main`. This branch is a self-contained home for the ESP8266 firmware and publishes its own
> OTA releases directly — no merge is needed.

## What's here
- **`esp8266/MyAthan_OTA/MyAthan_OTA.ino`** — the firmware (single-file Arduino sketch):
  prayer times from the AlAdhan API (Umm al-Qura by default), DST-correct timezone via
  `iso8601`, configurable calculation method / Asr school / high-latitude rule, per-prayer
  minute offsets, a local web UI at `myathan.local`, and self-update over the air.
- **`esp8266/libraries/JQ6500_Serial/`** — vendored audio-module library so CI builds are reproducible.
- **`.github/workflows/esp8266.yml`** — builds the sketch on every push to this branch and
  publishes `firmware.bin` + `version.txt` to the **`esp8266-latest`** pre-release tag.

## Hardware
ESP8266 (NodeMCU / ESP-12, CH340 USB-serial) + JQ6500 audio module. Pin wiring is documented
in the header of the sketch.

## Build & flash locally
```bash
arduino-cli core install esp8266:esp8266@3.1.2
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 --libraries esp8266/libraries esp8266/MyAthan_OTA
arduino-cli upload  -p COM5 --fqbn esp8266:esp8266:nodemcuv2 esp8266/MyAthan_OTA
```
Or open `esp8266/MyAthan_OTA/MyAthan_OTA.ino` in the Arduino IDE (board: **NodeMCU 1.0 (ESP-12E)**).

## Over-the-air updates
1. Bump `FW_VERSION` in `MyAthan_OTA.ino`.
2. Push to this branch — CI rebuilds and refreshes the `esp8266-latest` release.
3. On the device: `myathan.local` → **Device → Check for Updates**. It compares the published
   `version.txt` against its built-in `FW_VERSION` and flashes itself if a newer build exists.

The `esp8266-latest` tag is a **pre-release**, kept separate from the ESP32-C3 `v*` releases so
the two devices never download each other's binaries.
