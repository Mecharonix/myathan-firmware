# MyAthan USB installer

Install or recover the ESP8266 MyAthan box from a computer.

**Page:** https://mecharonix.github.io/myathan-firmware/

This page always installs the same firmware as GitHub **esp8266-latest**.

Current software: **2.2.11** (same as GitHub **esp8266-latest**). What changed in each version: see the `esp8266-ota` README changelog.

## When to use this page

Use USB if the box is new, stuck, or OTA says **Not Enough space**.

If the box is already on **2.2.9 or later**, prefer **http://myathan.local** → Check for Updates (or wait for 00:30). You do not need to USB-flash every update.

## How to flash

1. Chrome or Edge on a PC (not a phone).
2. Plug in USB. Windows may need the CH340 driver.
3. Connect, pick the USB/CH340 port (not Bluetooth).
4. Leave **Erase** unchecked if the box was already set up.
5. After it finishes, wait for home Wi‑Fi, or join **MyAthan Setup** and use http://192.168.4.1 if asked.
