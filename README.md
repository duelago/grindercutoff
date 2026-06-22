# GrinderCutoff

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
![ESP32-C6](https://img.shields.io/badge/ESP32-C6-blue)
![MQTT](https://img.shields.io/badge/MQTT-enabled-orange)
![BLE](https://img.shields.io/badge/BLE-MyScale_KP2048B-purple)

**Automatic grind-by-weight for "dumb" espresso grinders using a MyScale KP2048B scale, ESP32-C6, MQTT, and a Tasmota smart plug. Will only work with the ESP32C6 board because of the bluetooth implementation**

Stop your grinder automatically when the desired dose is reached—without modifying the grinder itself.

---

<img width="228" height="731" alt="Screenshot 2026-06-22 at 07 52 42" src="https://github.com/user-attachments/assets/94f0a867-318e-450a-b75b-d9a69efbf29c" />
<img width="3678" height="4308" alt="scale" src="https://github.com/user-attachments/assets/f6cebcb5-3111-45fe-9a01-1c23a2f6115a" />


## System Overview

```text
MyScale KP2048B  ──BLE──►  ESP32-C6  ──MQTT──►  Broker  ──MQTT──►  Tasmota Plug  ──► Grinder
                                │
                                └──WiFi──► Web Interface
```



## How It Works

1. Place the portafilter on the scale and tare it.
2. Start the grinder using the smart plug button.
3. GrinderCutoff detects that grinding has started.
4. The ESP32 continuously monitors the scale weight via BLE.
5. When the measured weight reaches Target Weightthe grinder is switched off automatically.
6. The system waits for remaining grounds to fall and calculates the final dose.
7. The adaptive algorithm refines the stop point over time.

---

## Hardware Requirements

| Component | Example |
|------------|---------|
| ESP32-C6 |  | https://www.aliexpress.com/item/1005008557621275.html
| Scale | MyScale KP2048B https://www.aliexpress.com/item/1005006441099756.html |
| Smart Plug | Any Tasmota-flashed plug https://www.aliexpress.com/item/1005006441099756.html |
| MQTT Broker | Mosquitto | (Will also work standalone without mqtt using http instead)
| WiFi Network | 2.4 GHz |


<img width="617" height="666" alt="Screenshot 2026-06-22 at 08 13 53" src="https://github.com/user-attachments/assets/09d3d1eb-a6cc-4c1f-b6b8-83ac52910654" />


<img width="1260" height="710" alt="Screenshot 2026-06-22 at 08 15 41" src="https://github.com/user-attachments/assets/54c38488-a9f1-42c3-8b43-c7e455af3b39" />


<img width="1286" height="654" alt="Screenshot 2026-06-22 at 08 16 42" src="https://github.com/user-attachments/assets/248e98f2-0f3d-4045-98d1-8a256f7e55f7" />


---

## Software Requirements

### Arduino IDE

Recommended:

- ESP32 Arduino Core 3.x
- Board: `ESP32C6 Dev Module`
- USB CDC On Boot: `Enabled`

### Libraries

| Library | Author |
|----------|---------|
| PubSubClient | knolleary |
| ESPAsyncWebServer | ESPHome |

The built-in ESP32 BLE implementation is used directly.

---

## Installation

### Clone the Repository

```bash
git clone https://github.com/YOUR_USERNAME/GrinderCutoff.git
cd GrinderCutoff
```

### Configure WiFi

```cpp
const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
```

### Configure MQTT

```cpp
char mqttServer[64]   = "192.168.1.x";
char mqttUser[32]     = "";
char mqttPass[32]     = "";
char tasmotaTopic[64] = "Tasmota_topic_xxx";
```

### Upload

Compile and flash using Arduino IDE.

---

## Web Interface

Default address:

```text
http://grindercutoff.local
```

### Live Status

- Current weight
- BLE connection status
- Grinder state
- Progress toward target weight

### Adjustable Settings

| Setting | Description |
|----------|-------------|
| Target Weight | Desired dose in grams |
| Pre-Offset | Early stop compensation |
| Learning Delay | Adaptive tuning parameter |
| MQTT Settings | Broker and topic configuration |

All settings can be modified without reflashing.

---

## MQTT Integration

### Commands

```text
cmnd/<topic>/POWER
```

### Status

```text
stat/<topic>/POWER
```

Example:

```text
cmnd/grinderplug/POWER ON
cmnd/grinderplug/POWER OFF
```

GrinderCutoff automatically reacts when the plug reports that the grinder has started.

---

## BLE Protocol

Based on reverse engineering work from:

- https://github.com/Zer0-bit/esp-arduino-ble-scales

### Service UUID

```text
0000FFB0-0000-1000-8000-00805F9B34FB
```

### Weight Notification UUID

```text
0000FFB2-0000-1000-8000-00805F9B34FB
```

### Tare Command UUID

```text
0000FFB1-0000-1000-8000-00805F9B34FB
```

---

## Example Workflow

```text
Target weight: 18.0 g
Pre-offset:    0.5 g

Grinder stops at:
17.5 g

Final settled weight:
18.0 g
```

---


## Inspired By

- GaggiMate
  https://github.com/jniebuhr/gaggimate

- ESP Arduino BLE Scales
  https://github.com/Zer0-bit/esp-arduino-ble-scales

---

## License

Released under the MIT License.
