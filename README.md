# GrinderCutoff ☕

Grind-by-weight för hemmabruk med **MyScale KP2048B**, **ESP32-C6** och en **Tasmota-styrd väggkontakt**. Kvarnen stängs av automatiskt när rätt mängd kaffe malts.

---

## Hur det fungerar

```
MyScale KP2048B  ──BLE──►  ESP32-C6  ──MQTT──►  Broker  ──MQTT──►  Tasmota-plug  ──►  Kvarn
                                │
                                └──WiFi──►  Webbgränssnitt (http://grindercutoff.local)
```

### Flöde per malning

1. Placera portafiltret på vågen och tara med **knappen på vågen**
2. Tryck på **knappen på Tasmota-pluggen** → kvarnen startar
3. ESP32:n detekterar via MQTT att relät slagits på och börjar övervaka vikten
4. När vikten når **målvikt − pre-offset** skickas `cmnd/<topic>/POWER OFF` → kvarnen stannar
5. Lyft bort portafiltret → redo för nästa malning

Ingen app behövs under normal användning. Webbgränssnittet används bara för att justera parametrar.

---

## Hårdvara

| Komponent | Detaljer |
|---|---|
| Mikrokontroller | ESP32-C6 (t.ex. Seeed Studio XIAO ESP32C6) |
| Våg | MyScale KP2048B |
| Relä/plug | Valfri Tasmota-flashad smart plug |
| MQTT-broker | t.ex. Mosquitto i Home Assistant |

---

## Mjukvara

### Arduino IDE-inställningar

- **Board:** `ESP32C6 Dev Module`
- **ESP32 Arduino core:** 3.x
- **Tools → USB CDC On Boot:** `Enabled` (krävs för Serial output)

### Bibliotek (installera via Library Manager)

| Bibliotek | Av |
|---|---|
| PubSubClient | knolleary |
| ESPAsyncWebServer | esphome (installera som ZIP från GitHub) |

Den inbyggda BLE-stacken (`BLEDevice`, `BLEScan`, `BLEClient`) används direkt från ESP32 Arduino core — inget extra BLE-bibliotek krävs.

---

## Installation

### 1. Klona repot

```bash
git clone https://github.com/<ditt-användarnamn>/GrinderCutoff.git
cd GrinderCutoff
```

### 2. Konfigurera WiFi och MQTT

Öppna `GrinderCutoff.ino` och uppdatera dessa rader:

```cpp
const char* WIFI_SSID     = "DIN_SSID";
const char* WIFI_PASSWORD = "DITT_LÖSENORD";

char mqttServer[64]   = "192.168.1.X";   // IP till din MQTT-broker
char mqttUser[32]     = "";              // lämna tomt om ingen autentisering
char mqttPass[32]     = "";
char tasmotaTopic[64] = "tasmota_XXXXX"; // Tasmota → Information → Topic
```

> **Tips:** Alla andra inställningar (målvikt, pre-offset, MQTT) kan ändras via webbgränssnittet efter att enheten startats — ingen ny flashning krävs.

### 3. Flasha ESP32-C6

Öppna sketchen i Arduino IDE och klicka **Upload**.

---

## Webbgränssnitt

När enheten startats är webbgränssnittet tillgängligt på:

```
http://grindercutoff.local
```

(eller via IP-adressen som skrivs ut i serial monitor)

### Live-vy

- Aktuell vikt med tre decimalers precision
- Tillståndsindikator: `⏸ VÄNTAR` / `🟢 MALER` / `🟠 SÄTTER SIG` / `✅ KLAR`
- Progressbar mot målvikt
- BLE- och relästatus

### Inställningar

| Parameter | Beskrivning |
|---|---|
| **Målvikt (g)** | Vikten att sikta mot, t.ex. `18.0` |
| **Pre-offset (g)** | Kvarnen stoppas X gram *före* målvikten för att kompensera för kaffe som fortsätter falla efter att motorn stannat. Börja med `0.5` och justera. |
| **Lärande delay** | Justerar pre-offset automatiskt baserat på faktisk slutvikt — lär sig din kvarns "inertia" efter ett par körningar. |

### MQTT-konfiguration

Server, port, topic, användarnamn och lösenord ändras direkt i webbgränssnittet och sparas i NVS-minnet utan att enheten behöver flashas om.

---

## BLE-protokoll (MyScale KP2048B)

Implementerat direkt utan externa bibliotek, baserat på [Zer0-bit/esp-arduino-ble-scales v4](https://github.com/Zer0-bit/esp-arduino-ble-scales/tree/v4).

| | UUID |
|---|---|
| Service | `0000FFB0-0000-1000-8000-00805F9B34FB` |
| Notify (vikt) | `0000FFB2-0000-1000-8000-00805F9B34FB` |
| Write (tara) | `0000FFB1-0000-1000-8000-00805F9B34FB` |

**Viktformat** (≥15 bytes):
```
Negativt: data[2] >> 4 == 0x8 eller 0xC
Råvärde:  (data[3] & 0x0F) << 24 | data[4] << 16 | data[5] << 8 | data[6]
Gram:     råvärde / 1000.0
```

**Tara-kommando** (20 bytes):
```
AC 40 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 D2 D2
```

---

## Tasmota-konfiguration

Enheten kommunicerar med Tasmota via MQTT med standard topic-format:

```
cmnd/<topic>/POWER   →  ON / OFF  (skicka kommando)
stat/<topic>/POWER   →  ON / OFF  (ta emot status)
```

ESP32:n prenumererar på `stat/<topic>/POWER` och startar viktövervakning automatiskt när Tasmota rapporterar att relät slagits på — oavsett om det sker via Tasmota-pluggens fysiska knapp, en Home Assistant-automation eller webbgränssnittet.

Hitta ditt Tasmota topic under: **Tasmota webbgränssnitt → Information → Topic**

---

## Inspirerat av

- [GaggiMate](https://github.com/jniebuhr/gaggimate) — state machine-arkitektur och adaptiv delay-justering
- [esp-arduino-ble-scales](https://github.com/Zer0-bit/esp-arduino-ble-scales) — MyScale BLE-protokoll

---

## Licens

MIT
