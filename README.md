# AREDN Message Board for ESP32

A lightweight message board designed for the **AREDN (Amateur Radio Emergency Data Network)**. It runs entirely on an Arduino Nano ESP32, providing a local chat hub for radio amateurs without requiring external internet or central server infrastructure.


<img src="https://github.com/jurgen178/aredn-msg-board/blob/main/doc/DSC_3227.JPG" alt="ESP32" width="500">

<img src="https://github.com/jurgen178/aredn-msg-board/blob/main/doc/DSC_3225.JPG" alt="ESP32" width="500">

https://bitfabrik.io/aredn-msg-board-firmware/

<img src="https://github.com/jurgen178/aredn-msg-board/blob/main/doc/web-flasher.png" alt="web flasher" width="500">

<img src="https://github.com/jurgen178/aredn-msg-board/blob/main/doc/msg-board-www.jpg" alt="ESP32" width="500">


## Key Technical Features

* **Resource-Conscious HTTP Polling:** The client browser polls the ESP32 every 30 seconds by default and requests only messages newer than the latest received ID when updating the feed.
* **Socket Optimization:** Every HTTP response strictly enforces `Connection: close` headers. This immediately frees up the limited TCP sockets on the ESP32, allowing the node to serve dozens of clients simultaneously without memory exhaustion.
* **Persistent FFat Storage:** Messages are stored as validated fixed-size binary records in the onboard Flash File System (FFat). JSON is used for import and export only.
* **Priority Tagging & Local Filtering:** Supports user-defined priority tiers (`Green`, `Orange - Warning`, `Red - Emergency`). Message rendering and filtering are executed 100% client-side inside the browser to keep network overhead to an absolute minimum.
* **Autonomous Node Architecture:** Features local browser-side input validation, client system clock synchronization, and JSON-based administrative logs export/import functionalities.

## API Documentation

### 1. Fetch Messages
* **Endpoint:** `GET /api/messages`
* **Optional parameters:** `limit=<n>`, `after=<id>`, `before=<id>`, `search=<term>`, `priority=green|orange|red|all`, `c_time=<epoch>`
* **Description:** Returns newest messages first. Use `after` for new messages, `before` for older messages, and `limit` to cap the page size.
* **Response Payload Format:**
```json
{"r":2,"f":true,"v":true,"s":"client","e":1790968087,"p":[{"i":10,"t":1790939700,"u":"W6AM","m":"Acknowledged. ..."
```

### 2. Submit Message
* **Endpoint:** `POST /api/messages`
* **Payload Format:**
```json
{
  "name": "Callsign/Name",
  "text": "Message payload",
  "priority": "orange" 
}
```

## System Requirements

* **Hardware:** Arduino Nano ESP32 (or equivalent ESP32 development board with sufficient flash memory mapped for FFat).
* **Software Libraries:** `ESPAsyncWebServer`, `Adafruit GFX Library`, and `Adafruit SSD1306`.
* **Client:** Any standard modern web browser (HTML5/ES6 support required for async/await execution).

## Installation

1. Install the ESP32 board package in the Arduino IDE.
2. Select an Arduino Nano ESP32 or a compatible ESP32 board with FFat support.
3. Install the following libraries through the Arduino Library Manager:
   * ESPAsyncWebServer
   * Adafruit GFX Library
   * Adafruit SSD1306
4. Copy the Wi-Fi settings into `arduino_secrets.h`:

```cpp
#define SECRET_SSID "your-network-name"
#define SECRET_PASS "your-network-password"
```

5. Open `aredn-service.ino`, select the correct board and port, then upload the sketch.

The service uses the ESP32's built-in `WiFi`, `Wire`, `FFat`, FreeRTOS and system libraries. `WebServer` and `ArduinoJson` are not required.

## Hardware

The optional SSD1306 OLED is connected through the board's default I2C interface. The display address used by the sketch is `0x3C`. The service can also run without the display; it reports the I2C device as unavailable and continues to start the web service.

### OLED Wiring

For an Arduino Nano ESP32, connect a 4-pin I2C SSD1306 OLED as follows:

| OLED pin | Nano ESP32 pin | Description |
| --- | --- | --- |
| `VCC` | `3V3` | 3.3 V supply |
| `GND` | `GND` | Ground |
| `SDA` | `A4` / `SDA` | I2C data |
| `SCL` | `A5` / `SCL` | I2C clock |

The display must use I2C address `0x3C`. Check the module's pin labels before connecting it; do not apply 5 V to a 3.3 V-only OLED module.

## Storage And Limits

Messages are stored in `/messages.dat` as fixed-size records with a checksum. Replacement files are written through temporary FFat files so that normal imports, edits and deletions do not modify the active file in place.

The in-memory index supports up to 4096 records. Imports currently retain at most 4095 messages; larger imports are accepted but truncated. The JSON fixtures in this project are intended for import and pagination testing.

Supported import fields are `id`, `created_at` in UTC ISO-8601 format (`YYYY-MM-DDTHH:MM:SSZ`), `name`, `text`, and optional `priority` (`green`, `orange`, or `red`).

## API

### Service Status

`GET /api/status` returns Wi-Fi, heap, FFat and board information.

`GET /api/time` returns the current device clock state.

### Admin API

The following endpoints require the admin session cookie created by login:

* `POST /api/admin/login` with form field `password`
* `POST /api/admin/logout`
* `GET /api/admin/status`
* `POST /api/admin/import` with an `application/json` file body
* `DELETE /api/admin/messages`
* `DELETE /api/admin/message?id=<id>`
* `POST /api/admin/message?id=<id>` with form fields `name`, `text`, and `priority`
* `POST /api/time` with form field `epoch`

### Export

`GET /api/messages/export.json` downloads the complete board as JSON.

### Examples

Submit a message:

```powershell
Invoke-WebRequest http://device-address/api/messages -Method Post -Body @{
  name = 'W6AM'
  text = 'Acknowledged.'
  priority = 'green'    # 'green' = normal, 'orange' = important, 'red' = urgent
  c_time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}
```

Read the newest messages:

```powershell
Invoke-RestMethod 'http://device-address/api/messages?limit=20'
```

Use `after=<id>` for newer messages and `before=<id>` for older messages. Additional filters are `search=<term>` and `priority=green|orange|red|all`.

## Security And Deployment

The service uses plain HTTP and is intended for a trusted local AREDN network. It does not provide TLS. Keep `arduino_secrets.h` out of shared source archives and change the admin password in the sketch before deployment.

## Load Test

`load-test.ps1` runs staged polling and posting tests against a configured device. Set `$BaseUri` at the top of the script before running it:

```powershell
.\load-test.ps1
```

The device clock must already be valid before running the posting stage. The script requires PowerShell and network access to the device.
