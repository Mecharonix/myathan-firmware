# MyAthan ESP8266 — how to update things

Read this before changing firmware, OTA, or the USB flasher. Wrong branch or wrong flash layout will brick OTA or wipe the ESP32-C3 project.

Repo: `Mecharonix/myathan-firmware`

---

## 1. Branches (do not merge these)

| Branch | What it is | What it is not |
|---|---|---|
| `main` | Legacy **ESP32-C3** project | Not the live prayer box |
| `esp8266-ota` | Source that GitHub Actions publishes to OTA tag **`esp8266-latest`** | Not the USB web page |
| `esp8266-web-flasher` | Chrome USB installer (GitHub Pages) | Does not hold the `.ino` |
| `esp8266-wip-bridge` | Working ESP8266 sketch with **4M2M**, **Wi‑Fi retry**, gzip UI | Not auto-published to `esp8266-latest` |

**Never merge** `esp8266-ota`, `esp8266-web-flasher`, or `esp8266-wip-bridge` into `main`. Those branches dropped ESP32-C3 files; a merge would delete that project from `main`.

**Never merge** the flasher branch into `esp8266-ota` either. They are different trees.

Clone each branch you work on into its **own folder**. Do not check out the flasher orphan branch on top of an `esp8266-ota` working copy.

```powershell
git clone -b esp8266-ota https://github.com/Mecharonix/myathan-firmware.git myathan-ota
git clone -b esp8266-web-flasher https://github.com/Mecharonix/myathan-firmware.git myathan-web-flasher
git clone -b esp8266-wip-bridge https://github.com/Mecharonix/myathan-firmware.git myathan-wip
```

---

## 2. What a device actually runs

- **OTA URL** (in the sketch):  
  `https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/version.txt`  
  and `firmware.bin` on the same tag.
- The device only installs if `FW_VERSION` on GitHub is **numerically newer** (dotted compare). Same version = no OTA, even if the binary changed.
- **USB** is how you force a new flash layout (4M3M → 4M2M) or recover a box that cannot OTA (`Not Enough space`).

Live OTA and USB are **not automatically the same binary**. Check both before you tell users to update.

---

## 3. USB flasher (this branch)

Public page: **https://mecharonix.github.io/myathan-firmware/**

| File | Role |
|---|---|
| `index.html` | User steps (Chrome, CH340, erase, Wi‑Fi portal) |
| `manifest.json` | ESP Web Tools; `version` is overwritten in CI |
| `.github/workflows/pages.yml` | Builds firmware + deploys Pages |

On every push to `esp8266-web-flasher` (or **Actions → Deploy web flasher → Run workflow**), CI:

1. Checks out this branch (the page).
2. Checks out **`esp8266-wip-bridge`** (the sketch).
3. Forces `#define OTA_USE_TEST_CHANNEL 0` so USB boxes pull **`esp8266-latest`**, not `esp8266-test`.
4. Packs **`control_full.html`** as the device UI.
5. Compiles  
   `esp8266:esp8266:nodemcuv2:eesz=4M2M,ssl=all`  
   (1MB OTA slots, full TLS so AlAdhan + GitHub HTTPS work).
6. Puts `firmware.bin` on the Pages site.

To change **page text**: edit `index.html` here, commit, push this branch.

To change **what USB installs**: change `esp8266-wip-bridge` (sketch / HTML / `pack_web.py`), **or** change the `ref:` / FQBN / patches in `pages.yml`, then push **this** branch so Pages rebuilds.

Watch the deploy:

```powershell
gh run list -R Mecharonix/myathan-firmware --branch esp8266-web-flasher --limit 3
```

GitHub Pages environment must allow this branch (Settings → Environments → `github-pages`). If deploy is rejected, add `esp8266-web-flasher` to the allowed branches or clear the branch policy.

### Tell users

- Chrome or Edge on a **PC**. Not Safari, not a phone.
- Windows: CH340 driver if no COM port.
- **Do not check Erase device** on an already-set-up box. Wi‑Fi, prayer offsets, calculation method, location, and volume live in EEPROM + the SDK Wi‑Fi area. A no-erase write only replaces the sketch. EEPROM address is the same on 4M3M and 4M2M (`0x3FB000`).
- Check Erase only for a factory wipe. Then they must join **MyAthan Setup** → `http://192.168.4.1`.

---

## 4. Change the working firmware (`esp8266-wip-bridge`)

Sketch: `esp8266/MyAthan_OTA/MyAthan_OTA.ino`

1. Edit on `esp8266-wip-bridge` only until you mean to ship OTA.
2. If devices should OTA this build later, bump:

   ```cpp
   #define FW_VERSION "x.y.z"
   ```

3. Keep **`OTA_USE_TEST_CHANNEL 0`** for anything that might go to customers. `1` is only for one USB’d test box that should read tag **`esp8266-test`**.
4. After you push wip, **rebuild the USB page** (empty commit or `workflow_dispatch` on `esp8266-web-flasher`). Wip push alone does **not** update the website.

USB CI currently compiles **`ssl=all`**. Do not switch USB back to `ssl=basic` unless you have confirmed AlAdhan + GitHub OTA still work. `ssl=basic` previously showed HTTP `-1` and `00:00` prayers on some units.

---

## 5. Ship an OTA (`esp8266-ota` → `esp8266-latest`)

Only do this when you **intend every already-updated box** to download the file.

1. Copy the intended sketch (and any `control.html` / `pack_web.py` / headers the CI needs) onto **`esp8266-ota`**.
2. Bump **`FW_VERSION`** above whatever is in  
   https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/version.txt
3. Compile FQBN must match the **flash layout already on the devices you care about**:
   - Boxes USB-flashed from this page: **`eesz=4M2M`** (need ~1MB OTA room).
   - Old 4M3M boxes: OTA of a ~500KB image often fails with **Not Enough space**. USB them first; do not rely on OTA to change 4M3M → 4M2M.
4. Use **`ssl=all`** once they are on 4M2M.
5. Push `esp8266-ota`. Workflow `.github/workflows/esp8266.yml` publishes `firmware.bin` + `version.txt` to tag **`esp8266-latest`**.
6. Wait ~30s after CI goes green before testing the download URL.

```powershell
git checkout esp8266-ota
# copy files, bump FW_VERSION, match FQBN to 4M2M if that is now the fleet
git add esp8266 .github/workflows/esp8266.yml
git commit -m "Release vX.Y.Z: why this update exists"
git push origin esp8266-ota
```

Confirm:

```powershell
(Invoke-WebRequest "https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/version.txt" -UseBasicParsing).Content
```

Devices pick it up at their OTA interval (wip-bridge: every 15 minutes; older images: daily **00:30**) or via the device page **Check for Updates**.

If USB boxes are still labeled the **same** `FW_VERSION` as `esp8266-latest`, they will **not** OTA, even if GitHub’s binary is different. That is useful (they will not fall back to an older revert). To push a new OTA you **must bump the version**.

---

## 6. Test channel (one box only)

- In the sketch: `#define OTA_USE_TEST_CHANNEL 1` → URLs use tag **`esp8266-test`**.
- USB-flash **only that box** with channel `1`. Leave fleet on `0` / `esp8266-latest`.
- Publish test bins to the `esp8266-test` release tag, not `esp8266-latest`.
- Set the flag back to **`0` before any fleet OTA**. The USB Pages workflow already forces `0` so the public flasher cannot ship a test-channel image by accident.

---

## 7. Local USB from Arduino (optional)

Board: **NodeMCU 1.0 (ESP-12E)**. Core **3.1.2**. Baud **9600**. Port is CH340 (not Bluetooth).

```text
FQBN: esp8266:esp8266:nodemcuv2:eesz=4M2M,ssl=all
```

Close Serial Monitor if upload says access denied.

---

## 8. Checklist before you call it shipped

- [ ] Right branch (wip vs ota vs flasher). No merge to `main`.
- [ ] `FW_VERSION` bumped **only** if OTA should move.
- [ ] OTA FQBN is **4M2M** for USB-updated boxes; `ssl=all`.
- [ ] `OTA_USE_TEST_CHANNEL` is `0` on anything public.
- [ ] Flasher site rebuilt if wip changed (`esp8266-web-flasher` workflow green).
- [ ] Pages still at https://mecharonix.github.io/myathan-firmware/
- [ ] Users told: Chrome, skip erase unless factory reset.

---

## 9. TL;DR

- **Edit sketch** on `esp8266-wip-bridge`.
- **Give people a USB installer** → push / re-run `esp8266-web-flasher` (builds wip as 4M2M `ssl=all`).
- **Give the fleet an OTA** → copy to `esp8266-ota`, bump version, 4M2M FQBN, push; CI updates `esp8266-latest`.
- **Do not** OTA a 4M3M layout onto boxes that still have no room. USB them first.
- **Do not** merge these branches into `main`.
