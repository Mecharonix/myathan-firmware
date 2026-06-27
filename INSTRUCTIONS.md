# MyAthan ESP8266 — Editing & Release Instructions

> Read this before touching the code. It explains what this is, how to build/flash it,
> and how to push a release so devices update themselves over the air (OTA).

---

## 1. What this is

This folder is the **ESP8266** firmware for the MyAthan prayer-reminder device
(prayer times via the AlAdhan API, JQ6500 audio module, a web UI at `http://myathan.local`,
and **self-update over the air from GitHub**).

- **Single file:** `MyAthan_OTA.ino` — everything lives here.
- It is the **active/real device**. There is an older **ESP32-C3** project that is **legacy** —
  see "Hands off the ESP32-C3" below.

### Hardware / build target
| Thing | Value |
|---|---|
| MCU board | ESP8266 — **NodeMCU 1.0 (ESP-12E)** |
| Arduino FQBN | `esp8266:esp8266:nodemcuv2` |
| ESP8266 core | `esp8266:esp8266@3.1.2` |
| Audio module | JQ6500 (library `JQ6500_Serial`, already installed under `Documents\Arduino\libraries`) |
| USB serial | CH340, usually **COM5** |
| Serial Monitor baud | **9600** |

---

## 2. GitHub repo & branch (IMPORTANT)

- Repo: **`Mecharonix/myathan-firmware`** (public).
- This firmware lives ONLY on the branch **`esp8266-ota`**, in the path
  `esp8266/MyAthan_OTA/MyAthan_OTA.ino`.
- The repo's **`main` branch is a completely different, legacy ESP32-C3 project.**

### ⚠️ Hands off the ESP32-C3 / never merge to main
- Do **not** edit, build, or depend on anything on `main`.
- Do **not** merge `esp8266-ota` into `main` — the branch had the ESP32-C3 files removed,
  so merging would **delete that project from main**. The branch is intentionally standalone.

This local folder (`C:\Users\saleh\Documents\Arduino\MyAthan_OTA\`) is a **working copy** for
editing/compiling/flashing. The source of truth for releases is the repo branch above.

---

## 3. When you edit the code

1. Make your change in `MyAthan_OTA.ino`.
2. **If you want devices to receive this change over the air, bump the version.** Near the top:
   ```cpp
   #define FW_VERSION "2.2.5"   // <-- increase this, e.g. "2.2.6"
   ```
   The device compares versions numerically (dotted, e.g. `2.2.6 > 2.2.5`) and only updates to a
   *newer* one. **If you don't bump it, no device will pull the update.**
3. Always compile before flashing/pushing (section 4).

The current version is whatever `FW_VERSION` says in the file right now.

---

## 4. Compile & flash over USB

You can use the Arduino IDE (open `MyAthan_OTA.ino`, board **NodeMCU 1.0 (ESP-12E)**, port **COM5**,
click Upload) — OR the bundled arduino-cli from PowerShell:

```powershell
$cli = "C:\Users\saleh\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$sketch = "C:\Users\saleh\Documents\Arduino\MyAthan_OTA"

# Compile
& $cli compile --fqbn esp8266:esp8266:nodemcuv2 "$sketch"

# Flash over USB (COM5). If "Access is denied", close the Arduino IDE Serial Monitor first:
Get-Process serial-monitor -ErrorAction SilentlyContinue | Stop-Process -Force
& $cli upload -p COM5 --fqbn esp8266:esp8266:nodemcuv2 "$sketch"
```

**When do you NEED to flash over USB?** Only when the device can't get the change via OTA:
- The first time on a fresh device.
- **If you change the OTA code itself** (`runOtaCheck`) and the version on the device is broken —
  a broken updater can't update itself, so you must USB-flash the fix once.

For normal changes, you don't need USB at all — just push (section 5) and let OTA deliver it.

---

## 5. Push a release (so devices update over the air)

The release lives in the repo, not this folder. Workflow:

```powershell
# 1. Clone the repo (once) and switch to the firmware branch
gh repo clone Mecharonix/myathan-firmware C:\path\to\clone
cd C:\path\to\clone
git checkout esp8266-ota

# 2. Copy your edited sketch into the repo
Copy-Item "C:\Users\saleh\Documents\Arduino\MyAthan_OTA\MyAthan_OTA.ino" `
          ".\esp8266\MyAthan_OTA\MyAthan_OTA.ino" -Force

# 3. Commit and push to the esp8266-ota branch
git add esp8266/MyAthan_OTA/MyAthan_OTA.ino
git commit -m "Describe your change; bump to vX.Y.Z"
git push origin esp8266-ota
```

Pushing triggers GitHub Actions (`.github/workflows/esp8266.yml`), which:
- compiles the sketch, and
- publishes `firmware.bin` + `version.txt` to the **`esp8266-latest`** pre-release tag.

Watch the build:
```powershell
gh run watch (gh run list -R Mecharonix/myathan-firmware --branch esp8266-ota --limit 1 --json databaseId --jq ".[0].databaseId") -R Mecharonix/myathan-firmware
```
> Note: right after CI finishes, the download URL can 404 for ~15-30s while GitHub propagates the
> new asset. Wait and retry.

The device then installs the new version:
- automatically at its **daily 00:30 check**, or
- immediately when someone opens `http://myathan.local` → **Device → Check for Updates**.

`gh` is authenticated as account **Saleh6969** (has `repo` + `workflow` scopes).

---

## 6. How OTA works (so you don't break it)

`runOtaCheck()` in the sketch does three HTTPS steps against the `esp8266-latest` release:
1. read `version.txt` and compare with `isNewer()`,
2. resolve GitHub's download redirect,
3. download `firmware.bin` and flash.

**Two ESP8266 landmines — keep these intact if you refactor OTA:**
1. **Follow the redirect manually.** GitHub serves `firmware.bin` as a **302** to
   `objects.githubusercontent.com`. `ESPhttpUpdate` does NOT follow it (fails with
   "Wrong HTTP Code"). We read the `Location` header ourselves and update from the resolved URL.
2. **Free the TLS buffer between steps.** The chip has only ~24 KB free heap; one
   `WiFiClientSecure` holds a ~16 KB buffer. Each HTTPS step runs in its own `{ }` scope so the
   buffer is released before the next — otherwise heap drops to ~6 KB and OTA fails.

The daily auto-check is at **00:30** on purpose (offset from the 00:00 prayer re-sync so they
don't collide) and is skipped while an athan is playing.

---

## 7. Quick checks

```powershell
# What version is the device running?
Invoke-RestMethod http://myathan.local/api/status | Select-Object fw, ota

# What version is published on GitHub?
(Invoke-WebRequest "https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/version.txt" -UseBasicParsing).Content

# Watch the device boot / OTA logs over USB (9600 baud) — opening the port resets the device.
```

---

## 8. TL;DR

- Edit `MyAthan_OTA.ino` → **bump `FW_VERSION`** → compile.
- Copy into a clone of `Mecharonix/myathan-firmware` (branch `esp8266-ota`) → commit → `git push`.
- CI publishes `esp8266-latest`; devices auto-update (daily 00:30) or via the web button.
- Only USB-flash when bootstrapping or when fixing the OTA code itself.
- Never touch `main` / the ESP32-C3 project. Never merge this branch into `main`.
