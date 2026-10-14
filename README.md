# MyAthan USB installer

Install or recover the ESP8266 MyAthan box from a computer.

**Page:** https://mecharonix.github.io/myathan-firmware/

This page always installs the same firmware as GitHub **esp8266-latest**.

## What’s in 2.2.10

- Volume stays at the level you set
- Joins saved Wi‑Fi after a power cut
- Setup page should open on **MyAthan Setup**
- **http://myathan.local** after it is on your network

## When to use this page

Use USB if the box is new, stuck, or OTA says **Not Enough space**.

If the box is already on **2.2.9 or 2.2.10**, prefer **http://myathan.local** → Check for Updates (or wait for the automatic check). You do not need to USB-flash every update.

## How to flash

1. Chrome or Edge on a PC (not a phone).
2. Plug in USB. Windows may need the CH340 driver.
3. Connect, pick the USB/CH340 port (not Bluetooth).
4. Leave **Erase** unchecked if the box was already set up.
5. After it finishes, wait for home Wi‑Fi, or join **MyAthan Setup** and use http://192.168.4.1 if asked.
