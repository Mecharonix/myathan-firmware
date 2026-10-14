# MyAthan (ESP8266)

Prayer times on the box, a web page on your phone, and updates from GitHub.

## What’s in 2.2.10

- Volume stays at the level you set (it no longer drops after each prayer)
- Joins your saved Wi‑Fi again after a power cut
- Setup page should open when you join **MyAthan Setup**
- Open the box at **http://myathan.local**
- Extra room on the chip for later updates (needs a USB flash once if OTA says there is not enough space)

## How to update

**USB installer (Chrome or Edge on a computer):**  
https://mecharonix.github.io/myathan-firmware/

Use USB if:

- this is a new box, or
- the screen / web page says **Not Enough space**, or
- the box is on older software and will not take an automatic update

Leave **Erase** unchecked if the box was already set up. Wi‑Fi, prayer offsets, and calculation method stay.

**If this box is already on 2.2.9 or 2.2.10** (you used the USB page, or it already has this software):  
it can update itself from GitHub. Open **http://myathan.local** → Check for Updates, or wait for the automatic check. You do not need USB every time.

**If the box is still on 2.2.8 or older**, do not wait for automatic OTA. Use the USB page once, then later updates can be automatic.

After USB, wait up to about a minute for home Wi‑Fi. If **MyAthan Setup** appears, join it (no password). The setup page should open; if not, go to http://192.168.4.1
