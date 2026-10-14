# MyAthan (ESP8266)

Prayer times on the box, a web page on your phone, and updates from GitHub.

Current software: **2.2.11**

## How to update

**USB installer (Chrome or Edge on a computer):**  
https://mecharonix.github.io/myathan-firmware/

Use USB if:

- this is a new box, or
- the screen / web page says **Not Enough space**, or
- the box is on older software and will not take an automatic update

Leave **Erase** unchecked if the box was already set up. Wi‑Fi, prayer offsets, and calculation method stay.

**If this box is already on 2.2.9 or later** (you used the USB page, or it already has this software):  
it can update itself from GitHub. Open **http://myathan.local** → Check for Updates, or wait for the automatic check at **00:30**. You do not need USB every time.

**If the box is still on 2.2.8 or older**, do not wait for automatic OTA. Use the USB page once, then later updates can be automatic.

After USB, wait up to about a minute for home Wi‑Fi. If **MyAthan Setup** appears, join it (no password). The setup page should open; if not, go to http://192.168.4.1

## Changelog

### 2.2.11
- Lights-before-athan slider on **http://myathan.local** (0–30 minutes, default 10; 0 = lights only when the athan starts)
- Automatic GitHub updates only at **00:30**, not throughout the day

### 2.2.10
- Volume stays at the level you set (it no longer drops after each prayer)

### 2.2.9
- Joins saved Wi‑Fi again after a power cut
- Setup page should open when you join **MyAthan Setup**
- **http://myathan.local** works again
- USB flash uses extra room on the chip so later updates can install over the air
