# Maintainers

Push firmware only on **`esp8266-ota`**. That publish refreshes **`esp8266-latest`** and the USB page.

Bump `FW_VERSION` or devices will not OTA.

The USB web page HTML lives on **`esp8266-web-flasher`**. Do not copy the sketch there.

Do not merge this branch into **`main`** (ESP32-C3).
