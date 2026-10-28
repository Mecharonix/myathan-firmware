<p align="center">
  <h1 align="center">MyAthan Firmware</h1>
  <p align="center">
    Smart Islamic prayer time device firmware for ESP32-C3
    <br />
    <em>On-device prayer calculation &bull; Hijri calendar &bull; 7 Islamic holidays &bull; Ramadan mode</em>
  </p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32--C3-blue" alt="Platform" />
  <img src="https://img.shields.io/badge/framework-Arduino-teal" alt="Framework" />
  <img src="https://img.shields.io/badge/build-PlatformIO-orange" alt="Build" />
  <img src="https://img.shields.io/badge/cost-~%244.15-green" alt="Cost" />
</p>

---

## Overview

MyAthan is an open-source smart prayer time device that plays the athan (Islamic call to prayer) at the correct time, entirely offline. It runs on a $4 ESP32-C3 microcontroller with a DFPlayer Mini for audio playback.

**Key principle:** The device calculates prayer times locally using astronomical algorithms. No cloud required for core operation. The cloud provides optional config sync, OTA updates, multi-room coordination, and analytics.

### What It Does

- Calculates all 5 daily prayer times using your GPS coordinates
- Plays athan audio from SD card at each prayer time
- Supports 7 calculation methods used worldwide
- Detects Ramadan automatically via Hijri calendar
- Plays special athan/doaa for 7 Islamic holidays
- Per-prayer volume scheduling (quieter Fajr at 5AM)
- Iqama countdown timer after athan
- Friday/Jumuah special athan for Dhuhr
- Power-loss recovery (resumes after outage)
- WiFi setup via Bluetooth or captive portal
- REST API for control from any device on your network
- Multi-room sync (multiple devices play simultaneously)

---

## Hardware

Commercial-board bring-up pinout for the ESP32-C3 SuperMini.

### Board

- ESP32-C3 SuperMini
- MAX98357A I2S amplifier
- Winbond 25Q64JVSIQ SPI flash
- 10x WS2812B LEDs
- 2x momentary push buttons (volume up, volume down)

### Wiring

| Component | Component pin | ESP32 pin | Description |
| --- | --- | --- | --- |
| Power | MAX98357 Vin and LED strip +5V | 5V | Main 5V power |
| Power | Winbond VCC | 3V3 | 3.3V flash power |
| Power | GND (all components) | GND | Shared common ground |
| Winbond 25Q64JVSIQ | CLK | Pin 4 | Hardware SPI clock |
| Winbond 25Q64JVSIQ | DO / MISO | Pin 5 | Hardware SPI MISO |
| Winbond 25Q64JVSIQ | DI / MOSI | Pin 6 | Hardware SPI MOSI |
| Winbond 25Q64JVSIQ | CS /CS | Pin 7 | Hardware SPI chip select |
| MAX98357A | BCLK | Pin 1 | I2S bit clock |
| MAX98357A | LRC / WS | Pin 10 | I2S word select |
| MAX98357A | DIN | Pin 3 | I2S audio data |
| WS2812B (10 LEDs) | DIN | Pin 0 | LED data (pin 0, same as WLED config) |
| Volume up button | signal | Pin 20 | Connects to GND when pressed |
| Volume down button | signal | Pin 21 | Connects to GND when pressed |

Button inputs are active-low (they connect to GND when pressed).

---

## Features

### Prayer Calculation (Fully Offline)

Calculates all prayer times on-device using the [PrayTimes.org](http://praytimes.org) algorithms. No internet required after initial location setup.

| Method | Fajr | Isha | Region |
|--------|------|------|--------|
| **ISNA** | 15deg | 15deg | North America |
| **MWL** | 18deg | 17deg | Europe, Asia, Americas |
| **Egyptian** | 19.5deg | 17.5deg | Egypt |
| **Umm al-Qura** | 18.5deg | 90min | Saudi Arabia |
| **Karachi** | 18deg | 18deg | Pakistan |
| **Tehran** | 17.7deg | 14deg | Iran |
| **Jafari** | 16deg | 14deg | Shia |

**ASR calculation:** Configurable — Standard (Shafi'i/Maliki/Hanbali) or Hanafi.

**High-latitude support** (above 48degN/S):
- Angle-based interpolation (recommended)
- Middle-of-night method
- One-seventh-of-night method

### Hijri Calendar & Islamic Holidays

Tabular Hijri calendar with +/-2 day manual adjustment. Automatically detects:

| Holiday | Date | Action |
|---------|------|--------|
| Eid al-Fitr | 1 Shawwal | Takbeer after each athan |
| Eid al-Adha | 10 Dhul Hijjah | Takbeer after each athan |
| Mawlid al-Nabi | 12 Rabi al-Awwal | Play doaa |
| Isra & Mi'raj | 27 Rajab | Configurable alert |
| Islamic New Year | 1 Muharram | Configurable alert |
| Ashura | 10 Muharram | Configurable alert |
| Laylat al-Qadr | 27 Ramadan | Special night alert |

Each holiday's behavior is independently configurable (enable/disable, custom track, post-athan track).

### Ramadan Mode

Auto-detected via Hijri calendar. When active:
- Special Fajr athan track
- **Suhoor alert** before Fajr (configurable: none / sound / LED / custom track)
- **Iftar alert** at Maghrib
- All configurable per user preference

### Audio & Volume

- Per-prayer athan track selection (different athan for each prayer)
- **Scheduled volume** — quieter Fajr (5AM), normal Dhuhr
- **Athan preview** — 10-second sample before committing
- **Doaa playback** — optional doaa after athan with configurable delay
- **Iqama timer** — countdown (0-60 min) with accelerating LED pulse

### LED Patterns

| State | Pattern | Meaning |
|-------|---------|---------|
| Idle | Brief flash every 5s | Device running normally |
| Pre-athan | Slow blink (1s) | Prayer time approaching |
| Playing | Solid on | Athan playing |
| Iqama countdown | Accelerating pulse | Countdown to iqama |
| Error | Fast blink (200ms) | Hardware/config error |
| No WiFi | Slow pulse (1.5s) | Not connected |
| Recovery | Double blink | Recovering from power loss |
| Provisioning | Triple blink | BLE/WiFi setup active |

### Button Controls

| Action | Gesture | Effect |
|--------|---------|--------|
| Manual trigger | Short press (<1s) | Play next prayer's athan |
| Preview | Double press | 10-second athan preview |
| WiFi reset | Long press (5s) | Erase WiFi, restart provisioning |

### REST API

Local HTTP server at `http://myathan.local` (mDNS):

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/status` | Device state, prayer times, Hijri date, Ramadan status |
| `GET` | `/timetable` | Today + tomorrow calculated prayer times |
| `POST` | `/trigger?prayer=N` | Manual athan (0=Fajr, 1=Dhuhr, 2=Asr, 3=Maghrib, 4=Isha) |
| `POST` | `/preview?track=N` | Play 10-second preview |
| `POST` | `/volume?level=N` | Set volume (0-30) |
| `GET` | `/config` | Read full device config |
| `POST` | `/config` | Update config (partial JSON merge) |
| `POST` | `/sync-trigger` | Multi-room synchronized playback |

### Multi-Room Sync

Multiple devices in the same group play athan simultaneously:
1. Devices sync clocks via NTP (accuracy ~10-20ms)
2. Cloud server computes a shared "play at epoch X" trigger
3. All devices play at exactly the same moment (within 50ms)

---

## Installation

### Prerequisites
- [PlatformIO CLI](https://platformio.org/install/cli) or [PlatformIO IDE](https://platformio.org/platformio-ide)
- Python 3.8+
- USB cable for ESP32-C3

### 1. Clone

```bash
git clone https://github.com/My-Athan/firmware.git
cd firmware
```

### 2. Prepare SD Card

Format a micro SD card as **FAT32** and copy MP3 files:

```
SD Card/
├── 0001.mp3    # Default athan
├── 0002.mp3    # Alternative athan
├── 0003.mp3    # Jumuah athan
├── 0004.mp3    # Fajr athan
├── 0005.mp3    # Iqama beep
├── 0006.mp3    # Suhoor alert
├── 0007.mp3    # Doaa
├── 0008.mp3    # Eid takbeer
└── ...         # Add as many as needed
```

Track numbers in config correspond to file numbers on the SD card.

### 3. Build & Flash

```bash
# Compile
pio run -e esp32c3

# Upload firmware
pio run -t upload -e esp32c3

# Upload default config to LittleFS
pio run -t uploadfs -e esp32c3

# Monitor serial output
pio device monitor -b 115200
```

### 4. First Boot Setup

1. Device starts in **provisioning mode** (LED triple-blinks)
2. **Option A — Bluetooth:** Open the [MyAthan PWA](https://app.myathan.com), tap "Set Up Device", select your device (MyAthan-XXXXXX), enter WiFi credentials
3. **Option B — Captive Portal:** After 60 seconds, device creates a WiFi hotspot. Connect to it, enter your WiFi credentials in the browser popup
4. Device connects to WiFi, syncs time via NTP, and starts calculating prayer times
5. Set your location via the PWA or REST API: `POST /config` with `{"location": {"lat": 21.42, "lon": 39.82, "timezone": "AST-3"}}`

---

## Configuration

Config stored in LittleFS at `/config.json` (schema v2). Key sections:

<details>
<summary><strong>Location & Calculation</strong></summary>

```json
{
  "location": {
    "lat": 21.4225,
    "lon": 39.8262,
    "city": "Mecca",
    "country": "SA",
    "timezone": "AST-3",
    "method": "MAKKAH",
    "asrJuristic": "standard",
    "highLatitudeRule": "angle_based"
  }
}
```
</details>

<details>
<summary><strong>Audio (per-prayer)</strong></summary>

```json
{
  "audio": {
    "volume": 20,
    "prayers": {
      "fajr": {
        "track": 4, "enabled": true, "volume": 12,
        "iqamaDelay": 20, "iqamaTrack": 5,
        "ramadanTrack": 6,
        "doaa": { "enabled": true, "track": 7, "delayMin": 3 }
      }
    }
  }
}
```
</details>

<details>
<summary><strong>Ramadan & Suhoor</strong></summary>

```json
{
  "ramadan": {
    "enabled": true,
    "suhoorAlertMinutes": 30,
    "suhoorMode": "sound",
    "suhoorTrack": 6,
    "suhoorLed": true,
    "iftarAlertEnabled": true,
    "iftarTrack": 0
  }
}
```
</details>

<details>
<summary><strong>Holidays</strong></summary>

```json
{
  "holidays": {
    "eidFitr": { "enabled": true, "track": 0, "postAthanTrack": 8 },
    "eidAdha": { "enabled": true, "track": 0, "postAthanTrack": 8 },
    "mawlid": { "enabled": true, "track": 7 },
    "laylatAlQadr": { "enabled": true, "track": 0 }
  }
}
```
</details>

<details>
<summary><strong>Schedule & Multi-Room</strong></summary>

```json
{
  "schedule": { "fridayJumuah": true, "jumuahTrack": 3 },
  "hijri": { "adjustment": 0 },
  "multiRoom": { "groupId": "", "syncOffsetMs": 0 }
}
```
</details>

Full schema: [`data/config.json`](data/config.json)

---

## Project Structure

```
firmware/
├── src/
│   ├── main.cpp                        # Entry point — wires all managers
│   ├── config/
│   │   ├── ConfigManager.h/.cpp        # LittleFS JSON config (v2 + migration)
│   │   └── defaults.h                  # All constants and pin definitions
│   ├── wifi/
│   │   └── WifiProvisioner.h/.cpp      # BLE GATT + WiFiManager fallback
│   ├── time/
│   │   └── NtpSync.h/.cpp             # Timezone-aware NTP sync
│   ├── audio/
│   │   └── AudioManager.h/.cpp         # DFPlayer Mini UART driver
│   ├── led/
│   │   └── LedManager.h/.cpp           # 9-state non-blocking LED FSM
│   ├── input/
│   │   └── ButtonHandler.h/.cpp        # Debounced short/double/long press
│   ├── prayer/
│   │   ├── PrayerCalculator.h/.cpp     # On-device prayer times (7 methods)
│   │   ├── HijriCalendar.h/.cpp        # Tabular Hijri conversion
│   │   ├── HolidayHandler.h/.cpp       # 7 Islamic holidays
│   │   ├── PrayerScheduler.h/.cpp      # Master scheduler
│   │   └── IqamaTimer.h/.cpp           # Per-prayer countdown
│   ├── api/
│   │   └── LocalServer.h/.cpp          # REST API + mDNS
│   ├── sync/
│   │   └── MultiRoomSync.h/.cpp        # Cloud-only group sync
│   ├── net/
│   │   ├── BackendClient.h/.cpp        # Cloud API client
│   │   └── OfflineCache.h/.cpp         # 7-day timetable cache
│   └── ota/
│       └── OtaManager.h/.cpp           # Dual-partition OTA with rollback
├── test/
│   ├── test_prayer_calculator/         # 10 tests — cities, methods, high-lat
│   └── test_hijri_calendar/            # 18 tests — dates, holidays, leap years
├── data/
│   └── config.json                     # Default config (v2 schema)
├── include/
│   └── myathan_version.h               # Firmware version
├── platformio.ini                      # Build configuration
├── CLAUDE.md                           # AI assistant project context
└── .claude/skills/                     # Claude Code slash commands
```

---

## Development

```bash
# Build
pio run -e esp32c3

# Flash firmware
pio run -t upload -e esp32c3

# Flash filesystem (config.json)
pio run -t uploadfs -e esp32c3

# Serial monitor
pio device monitor -b 115200

# Run unit tests
pio test -e esp32c3
```

### Claude Code Skills

| Command | Description |
|---------|-------------|
| `/build` | Compile firmware |
| `/test` | Run unit tests |
| `/flash` | Upload to device + monitor |
| `/review` | ESP32-specific code review checklist |
| `/prayer-verify` | Verify prayer times against Aladhan API |

---

## Related

| Repository | Description |
|------------|-------------|
| [**core**](https://github.com/My-Athan/core) | Backend API, mobile PWA, admin dashboard |

## License

Copyright 2026 MyAthan Contributors.
