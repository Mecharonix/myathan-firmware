# MyAthan web flasher

Chrome / Edge USB installer for ESP8266 MyAthan boxes. Lives only on `esp8266-web-flasher`.

- Do **not** merge this into `main` or `esp8266-ota`.
- Each deploy compiles `esp8266-wip-bridge` as a USB image: 4M2M OTA slots, Wi‑Fi retry, full UI, production OTA channel (`esp8266-latest`), `ssl=all`.
- That is not the GitHub `esp8266-latest` 2.2.8 revert (4M3M, no Wi‑Fi retry).
- Site: after Pages deploys, open `https://mecharonix.github.io/myathan-firmware/`
