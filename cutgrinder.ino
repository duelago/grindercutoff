/*
 * GrinderCutoff - ESP32-C6 Arduino Sketch
 * =========================================
 * Läser vikt från MyScale KP2048B via BLE och bryter strömmen
 * till en Tasmota-plug via MQTT när vikten uppnår ett förinställt värde.
 *
 * Board: "ESP32C6 Dev Module", ESP32 Arduino core 3.x
 * Tools → USB CDC On Boot → Enabled för Serial output!
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>

// Forward declarations
void bleTare();
void savePrefs();
void startBleScan();
void onWeightReceived(float weight);

// ─── Konfiguration ────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Alter_3G";
const char* WIFI_PASSWORD = "xxx";
char mqttServer[64]    = "192.168.7.58";
char mqttUser[32]      = "duelago";
char mqttPass[32]      = "xxx";
int  mqttPort          = 1883;
char tasmotaTopic[64]  = "sonoff";

float targetWeight  = 18.0f;
float preoffset     = 0.5f;
bool  autoTare      = true;
bool  autoRestart   = true;
float grindDelay       = 300.0f;
bool  delayAdjust      = true;
float lastActualWeight = 0.0f;
int   grindCount       = 0;

// ─── Tillståndsmaskin ─────────────────────────────────────────────────────────
enum GrindState {
  STATE_IDLE, STATE_TARING, STATE_WAITING_STABLE,
  STATE_GRINDING, STATE_SETTLING, STATE_DONE
};
GrindState grindState        = STATE_IDLE;
unsigned long stateEnterTime = 0;
unsigned long tareTimeout    = 3000;
unsigned long settleTimeout  = 3000;
float   currentWeight   = 0.0f;
float   peakWeight      = 0.0f;
bool    relayOn         = false;
bool    scalesConnected = false;
unsigned long lastWeightTime = 0;

// ─── BLE ──────────────────────────────────────────────────────────────────────
#define MYSCALE_SERVICE_UUID  "0000ffb0-0000-1000-8000-00805f9b34fb"
#define MYSCALE_NOTIFY_UUID   "0000ffb2-0000-1000-8000-00805f9b34fb"
#define MYSCALE_WRITE_UUID    "0000ffb1-0000-1000-8000-00805f9b34fb"

// Protokoll-bytes från myscale.cpp
static const uint8_t TARE_CMD[] = {
  0xAC, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xD2, 0xD2
};

Preferences         prefs;
WiFiClient          wifiClient;
PubSubClient        mqttClient(wifiClient);
AsyncWebServer      webServer(80);

BLEScan* bleScan      = nullptr;
BLEClient* bleClient    = nullptr;
BLERemoteCharacteristic* notifyChar = nullptr;
BLERemoteCharacteristic* writeChar  = nullptr;
BLEAdvertisedDevice* foundDevice = nullptr;
bool deviceFound = false;

// Debug-läge: sätt true för att skriva ut alla råbytes från vågen
bool rawDebug = true;

// ─── Hjälpfunktioner ─────────────────────────────────────────────────────────
float stopAt() { return targetWeight - preoffset; }

void enterState(GrindState newState) {
  grindState     = newState;
  stateEnterTime = millis();
  const char* names[] = {"IDLE","TARING","WAITING_STABLE","GRINDING","SETTLING","DONE"};
  Serial.printf("[STATE] → %s\n", names[newState]);
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
void sendTasmotaCommand(const char* cmd) {
  if (!mqttClient.connected()) return;
  char topic[80];
  snprintf(topic, sizeof(topic), "cmnd/%s/POWER", tasmotaTopic);
  mqttClient.publish(topic, cmd);
  Serial.printf("[MQTT] %s → %s\n", topic, cmd);
}

void relayOff() {
  if (relayOn) { 
    sendTasmotaCommand("OFF");
    relayOn = false; 
    Serial.println("[RELAY] OFF"); 
  }
}

void relayTurnOn() {
  sendTasmotaCommand("ON"); 
  relayOn = true; 
  peakWeight = 0.0f; 
  Serial.println("[RELAY] ON");
}

// ─── Adaptiv delay ───────────────────────────────────────────────────────────
void adjustDelay(float actual, float target) {
  if (!delayAdjust || grindCount < 2) return;
  float error      = actual - target;
  float adjustment = constrain(error * 50.0f, -500.0f, 500.0f);
  grindDelay       = constrain(grindDelay + adjustment, 0.0f, 3000.0f);
  Serial.printf("[ADAPT] Faktisk=%.2fg Mål=%.2fg → delay=%.0fms\n", actual, target, grindDelay);
  prefs.begin("grinder", false);
  prefs.putFloat("grindDelay", grindDelay);
  prefs.end();
}

// ─── BLE vikt-callback ───────────────────────────────────────────────────────
void onWeightReceived(float weight) {
  currentWeight  = weight;
  lastWeightTime = millis();
  if (grindState == STATE_GRINDING && weight > peakWeight) peakWeight = weight;
  
  switch (grindState) {
    case STATE_IDLE:
      // Om du har nollställt vågen och startar malningen, kommer vikten att börja stiga.
      // När den passerar 1.5g och relät är igång, hoppar vi automatiskt in i malningsläget!
      if (weight > 1.5f && relayOn) {
        Serial.println("[AUTO] Detekterade ökande vikt — startar övervakning (STATE_GRINDING)");
        enterState(STATE_GRINDING);
      }
      break;
      
    case STATE_TARING:
    case STATE_WAITING_STABLE:
      // Om en tare ändå skulle triggas, se till att vi bara faller tillbaka till IDLE
      // utan att röra relät eller starta kvarnen.
      enterState(STATE_IDLE);
      break;
      
    case STATE_GRINDING:
      Serial.printf("[GBW] %.2fg / %.2fg\n", weight, targetWeight);
      // Bryt strömmen när vi passerar målet minus pre-offset
      if (weight >= stopAt()) { 
        relayOff(); 
        lastActualWeight = weight; 
        enterState(STATE_SETTLING); 
      }
      break;
      
    case STATE_SETTLING:
      // Väntar ut utjämningstiden i loop()
      break;
      
    case STATE_DONE:
      // När du lyfter bort det fyllda portafiltret blir vikten negativ (t.ex. -20g).
      // Då slår vi PÅ relät igen så att kvarnen har ström inför nästa gång!
      if (autoRestart && weight < -4.0f) { 
        Serial.println("[AUTO] Portafilter bortlyft. Slår PÅ relät för nästa malning.");
        relayTurnOn();
        enterState(STATE_IDLE); 
      }
      break;
  }
}

// ─── BLE notifikations-callback ──────────────────────────────────────────────
void notifyCallback(BLERemoteCharacteristic* c, uint8_t* data, size_t length, bool isNotify) {
  if (rawDebug) {
    Serial.printf("[RAW %d] ", (int)length);
    for (size_t i = 0; i < length; i++) Serial.printf("%02X ", data[i]);
    Serial.println();
  }

  if (length < 15) {
    Serial.printf("[BLE]  Paket för kort (%d bytes), ignorerar\n", (int)length);
    return;
  }

  bool isNegative = ((data[2] >> 4) == 0x8 || (data[2] >> 4) == 0xC);
  uint32_t raw =
    ((uint32_t)(data[3] & 0x0F) << 24) |
    ((uint32_t)data[4] << 16) |
    ((uint32_t)data[5] <<  8) |
    ((uint32_t)data[6]);
  int32_t signed_raw = isNegative ? -(int32_t)raw : (int32_t)raw;
  float weight = (float)signed_raw / 1000.0f;
  Serial.printf("[BLE]  Vikt: %.3fg (raw=%d, neg=%d)\n", weight, signed_raw, isNegative);
  onWeightReceived(weight);
}

// ─── BLE tara ────────────────────────────────────────────────────────────────
void bleTare() {
  if (writeChar && scalesConnected) {
    writeChar->writeValue((uint8_t*)TARE_CMD, sizeof(TARE_CMD), false);
    Serial.println("[BLE]  Tara skickat");
  }
}

void sendTare() {
  if (scalesConnected) {
    bleTare();
    enterState(STATE_TARING);
    Serial.println("[TARE] Väntar på bekräftelse...");
  }
}

// ─── BLE scan ────────────────────────────────────────────────────────────────
void startBleScan() {
  scalesConnected = false;
  deviceFound     = false;
  if (foundDevice) { delete foundDevice; foundDevice = nullptr; }
  notifyChar = nullptr;
  writeChar  = nullptr;
  bleScan->clearResults();
  bleScan->start(5, nullptr, true);
  Serial.println("[BLE]  Scanning 5s...");
}

// ─── BLE anslutning ──────────────────────────────────────────────────────────
bool connectToScale() {
  if (!foundDevice) return false;
  Serial.printf("[BLE]  Ansluter till %s...\n", foundDevice->getAddress().toString().c_str());

  if (bleClient) { delete bleClient; bleClient = nullptr; }
  bleClient = BLEDevice::createClient();
  if (!bleClient->connect(foundDevice)) {
    Serial.println("[BLE]  ✗ Anslutning misslyckades");
    return false;
  }
  Serial.println("[BLE]  ✓ TCP-ansluten, hämtar tjänster...");
  delay(200);

  BLERemoteService* svc = bleClient->getService(MYSCALE_SERVICE_UUID);
  if (!svc) { 
    Serial.println("[BLE]  ✗ Tjänst FFB0 saknas"); 
    bleClient->disconnect(); 
    return false; 
  }

  notifyChar = svc->getCharacteristic(MYSCALE_NOTIFY_UUID);
  writeChar  = svc->getCharacteristic(MYSCALE_WRITE_UUID);
  if (!notifyChar || !writeChar) { 
    Serial.println("[BLE]  ✗ Karakteristik saknas"); 
    bleClient->disconnect(); 
    return false;
  }

  if (notifyChar->canNotify()) {
    // Eftersom registerForNotify returnerar 'void' kör vi den som en ren kommandorad
    notifyChar->registerForNotify(notifyCallback, true);
    Serial.println("[BLE]  ✓ Notifikationer registrerade!");
  } else {
    Serial.println("[BLE]  ✗ Notify stöds ej"); 
    bleClient->disconnect(); 
    return false;
  }
  
  delay(300);
  Serial.println("[BLE]  ✓ Anslutning klar - väntar på vikt-data...");
  scalesConnected = true;
  return true;
}

// ─── BLE scan callback ───────────────────────────────────────────────────────
class MyScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) {
    bool found = false;
    if (device.haveName()) {
      String name = device.getName().c_str();
      if (name == "MY_SCALE" || name == "my_scale" || name == "blackcoffee") {
        Serial.printf("\n[BLE]  Hittad via namn: \"%s\"\n", name.c_str());
        found = true;
      }
    }
    if (!found && device.haveServiceUUID() &&
        device.isAdvertisingService(BLEUUID(MYSCALE_SERVICE_UUID))) {
      Serial.println("\n[BLE]  Hittad via service UUID!");
      found = true;
    }
    if (!found) {
      String mac = String(device.getAddress().toString().c_str());
      mac.toLowerCase();
      if (mac == "d0:4d:00:6e:2a:91") {
        Serial.println("\n[BLE]  Hittad via MAC!");
        found = true;
      }
    }
    if (found && !deviceFound) {
      bleScan->stop();
      if (foundDevice) delete foundDevice;
      foundDevice = new BLEAdvertisedDevice(device);
      deviceFound = true;
    }
  }
};

// ─── HTML ─────────────────────────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sv">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>GrinderCutoff</title>
  <style>
    *{box-sizing:border-box}
    body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#eee;
         display:flex;flex-direction:column;align-items:center;padding:20px;margin:0}
    h1{color:#e94560;margin-bottom:4px}
    .sub{color:#888;font-size:.85em;margin-bottom:24px}
    .card{background:#16213e;border-radius:12px;padding:20px;width:100%;max-width:420px;
          margin-bottom:16px;box-shadow:0 4px 15px rgba(0,0,0,.4)}
    .card h2{margin:0 0 14px;font-size:.9em;color:#e94560;text-transform:uppercase;letter-spacing:1px}
    .big{font-size:3.5em;font-weight:bold;text-align:center;color:#4ecca3;margin:8px 0}
    .state-badge{text-align:center;font-size:.8em;padding:4px 14px;border-radius:20px;
    display:inline-block;margin:4px auto}
    .row{display:flex;gap:10px}.row>div{flex:1}
    .status-row{display:flex;gap:10px;justify-content:center;margin-top:8px;flex-wrap:wrap}
    .badge{padding:4px 12px;border-radius:20px;font-size:.8em;font-weight:bold}
    .ble-ok{background:#0f3460}.ble-err{background:#5c2020}
    .rel-on{background:#2a7a4b}.rel-off{background:#7a2a2a}
    label{display:block;margin-bottom:4px;font-size:.85em;color:#aaa}
    input[type=number],input[type=text],input[type=password]{width:100%;padding:10px;border-radius:8px;
    border:1px solid #0f3460;background:#0d1b2a;color:#eee;font-size:1em;margin-bottom:12px}
    .toggle-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
    .toggle-row label{margin:0;color:#ccc}
    input[type=checkbox]{width:20px;height:20px}
    button{width:100%;padding:12px;border:none;border-radius:8px;font-size:1em;
    font-weight:bold;cursor:pointer;margin-bottom:8px;transition:opacity .2s}
    button:hover{opacity:.85}
    .btn-save{background:#e94560;color:#fff}
    .btn-relay{background:#4ecca3;color:#1a1a2e}
    .btn-tare{background:#0f3460;color:#fff}
    .btn-start{background:#6c3ac7;color:#fff}
    .pw{background:#0d1b2a;border-radius:8px;height:16px;overflow:hidden;margin-top:8px}
    .pb{height:100%;background:linear-gradient(90deg,#4ecca3,#e94560);transition:width .5s;border-radius:8px}
    .msg{color:#4ecca3;font-size:.85em;text-align:center;min-height:18px;margin-top:4px}
    .stat-table{width:100%;font-size:.85em;border-collapse:collapse}
    .stat-table td{padding:4px 0;color:#aaa}.stat-table td:last-child{color:#fff;text-align:right}
  </style>
</head>
<body>
  <h1>☕ GrinderCutoff</h1>
  <p class="sub">MyScale KP2048B + Tasmota Grind-by-Weight</p>

  <div class="card">
    <h2>Live</h2>
    <div class="big" id="weightVal">-.--</div>
    <div style="text-align:center">
      <span class="state-badge" id="stateBadge" style="background:#333">–</span>
    </div>
    <div class="pw"><div class="pb" id="progressBar" style="width:0%"></div></div>
    
    <div class="status-row">
      <span class="badge ble-err" id="bleBadge">BLE: söker...</span>
      <span class="badge rel-off" id="relayBadge">Relä: AV</span>
    </div>
  </div>

  <div class="card">
    <h2>Statistik</h2>
    <table class="stat-table">
      <tr><td>Antal malningar</td><td id="grindCount">0</td></tr>
      <tr><td>Lärd delay</td><td id="grindDelay">300 ms</td></tr>
      <tr><td>Senaste slutvikt</td><td id="lastActual">–</td></tr>
      <tr><td>Stoppvikt (mål - offset)</td><td id="stopAtVal">–</td></tr>
    </table>
  </div>

  <div class="card">
    <h2>Inställningar</h2>
    <div class="row">
      <div><label>Målvikt (g)</label>
        <input type="number" id="targetWeight" step="0.1" min="0.1" value="18.0"></div>
      <div><label>Pre-offset (g) 🔧</label>
        <input type="number" id="preoffset" step="0.1" min="0" value="0.5"></div>
    </div>
    <div class="toggle-row"><label>Auto-tara + start vid portafilter</label>
      <input type="checkbox" id="autoTare" checked></div>
    <div class="toggle-row"><label>Auto-redo vid bortlyft portafilter</label>
      <input type="checkbox" id="autoRestart" checked></div>
    <div class="toggle-row"><label>Lärande delay-justering</label>
      <input type="checkbox" id="delayAdjust" checked></div>
    <button class="btn-save" onclick="saveSettings()">💾 Spara inställningar</button>
    <div class="msg" id="saveMsg"></div>
  </div>

  <div class="card">
    <h2>MQTT / Tasmota</h2>
    <label>MQTT-server (IP)</label>
    <input type="text" id="mqttServer" placeholder="192.168.1.X">
    <div class="row">
      <div><label>Port</label>
        <input type="number" id="mqttPort" value="1883" min="1" max="65535"></div>
      <div><label>Tasmota topic</label>
        <input type="text" id="tasmotaTopic" placeholder="sonoff"></div>
    </div>
    <div class="row">
      <div><label>Användarnamn</label>
        <input type="text" id="mqttUser" placeholder="(tomt = ingen auth)"></div>
      <div><label>Lösenord</label>
        <input type="password" id="mqttPass" placeholder="(tomt = ingen auth)"></div>
    </div>
    <button class="btn-save" onclick="saveMqtt()">💾 Spara MQTT</button>
    <div class="msg" id="mqttMsg"></div>
  </div>

  <div class="card">
    <h2>Manuell kontroll</h2>
    <button class="btn-start" onclick="startGrind()">▶ Starta malning (tara + kör)</button>
    <button class="btn-relay" onclick="toggleRelay()">⚡ Växla relä manuellt</button>
    <button class="btn-tare"  onclick="tare()">⚖️ Tara våg</button>
    <button class="btn-tare" style="background:#444" onclick="resetState()">↺ Återställ tillstånd</button>
    <div class="msg" id="ctrlMsg"></div>
  </div>

<script>
  const STATES = {
    'IDLE':'🟤 VÄNTAR','TARING':'🟡 TARAR','WAITING_STABLE':'🟡 STABILISERAR',
    'GRINDING':'🟢 MALER','SETTLING':'🟠 SÄTTER SIG','DONE':'✅ KLAR'
  };
  const STATE_COLORS = {
    'IDLE':'#444','TARING':'#7a6000','WAITING_STABLE':'#7a6000',
    'GRINDING':'#2a7a4b','SETTLING':'#7a5500','DONE':'#0f5c3a'
  };
  const mqttFields = ['mqttServer','mqttPort','tasmotaTopic','mqttUser','mqttPass'];
  function mqttCardFocused() { return mqttFields.includes(document.activeElement.id); }

  function fetchStatus() {
    fetch('/status').then(r=>r.json()).then(d=>{
      document.getElementById('weightVal').textContent = d.weight.toFixed(2)+' g';
      const pct = Math.min(100, d.weight/Math.max(0.01,d.stopAt)*100);
      document.getElementById('progressBar').style.width = pct+'%';
      document.getElementById('bleBadge').textContent  = d.ble ? 'BLE: ansluten ✓' : 'BLE: söker...';
      document.getElementById('bleBadge').className    = 'badge '+(d.ble?'ble-ok':'ble-err');
      document.getElementById('relayBadge').textContent= d.relay ? 'Relä: PÅ' : 'Relä: AV';
      document.getElementById('relayBadge').className  = 'badge '+(d.relay?'rel-on':'rel-off');
      const sb = document.getElementById('stateBadge');
      sb.textContent = STATES[d.state] || d.state; sb.style.background = STATE_COLORS[d.state] || '#333';
      document.getElementById('grindCount').textContent = d.grindCount;
      document.getElementById('grindDelay').textContent = d.grindDelay.toFixed(0)+' ms';
      document.getElementById('lastActual').textContent = d.lastActual>0 ? d.lastActual.toFixed(2)+' g' : '–';
      document.getElementById('stopAtVal').textContent  = d.stopAt.toFixed(2)+' g';
      document.getElementById('targetWeight').value = d.targetWeight;
      document.getElementById('preoffset').value    = d.preoffset;
      document.getElementById('autoTare').checked   = d.autoTare;
      document.getElementById('autoRestart').checked= d.autoRestart;
      document.getElementById('delayAdjust').checked= d.delayAdjust;
      if (!mqttCardFocused()) {
        document.getElementById('mqttServer').value   = d.mqttServer;
        document.getElementById('mqttPort').value     = d.mqttPort;
        document.getElementById('tasmotaTopic').value = d.tasmotaTopic;
        document.getElementById('mqttUser').value     = d.mqttUser;
      }
    }).catch(()=>{});
  }

  function saveSettings(){
    const b = new URLSearchParams({
      targetWeight:document.getElementById('targetWeight').value,
      preoffset:   document.getElementById('preoffset').value,
      autoTare:    document.getElementById('autoTare').checked?'1':'0',
      autoRestart: document.getElementById('autoRestart').checked?'1':'0',
      delayAdjust: document.getElementById('delayAdjust').checked?'1':'0'
    });
    fetch('/settings',{method:'POST',body:b}).then(r=>r.text()).then(t=>{
      document.getElementById('saveMsg').textContent=t;
      setTimeout(()=>document.getElementById('saveMsg').textContent='',3000);
    });
  }
  function saveMqtt(){
    const b = new URLSearchParams({
      mqttServer:   document.getElementById('mqttServer').value,
      mqttPort:     document.getElementById('mqttPort').value,
      tasmotaTopic: document.getElementById('tasmotaTopic').value,
      mqttUser:     document.getElementById('mqttUser').value,
      mqttPass:     document.getElementById('mqttPass').value
    });
    fetch('/mqtt',{method:'POST',body:b}).then(r=>r.text()).then(t=>{
      document.getElementById('mqttMsg').textContent=t;
      setTimeout(()=>document.getElementById('mqttMsg').textContent='',4000);
    });
  }
  function startGrind(){api('/grind/start','ctrlMsg');}
  function toggleRelay(){api('/relay/toggle','ctrlMsg');}
  function tare(){api('/tare','ctrlMsg');}
  function resetState(){api('/reset','ctrlMsg');}
  function api(url,msgId){
    fetch(url).then(r=>r.text()).then(t=>{
      document.getElementById(msgId).textContent=t;
      setTimeout(()=>document.getElementById(msgId).textContent='',3000);
    });
  }
  fetchStatus();
  setInterval(fetchStatus,600);
</script>
</body>
</html>
)rawliteral";

// ─── Spara / ladda inställningar ─────────────────────────────────────────────
void savePrefs() {
  prefs.begin("grinder", false);
  prefs.putFloat("targetW",    targetWeight);
  prefs.putFloat("preoffset",  preoffset);
  prefs.putFloat("grindDelay", grindDelay);
  prefs.putInt("grindCount",   grindCount);
  prefs.putBool("autoTare",    autoTare);
  prefs.putBool("autoRestart", autoRestart);
  prefs.putBool("delayAdjust", delayAdjust);
  prefs.putString("mqttServer", mqttServer);
  prefs.putString("mqttUser",   mqttUser);
  prefs.putString("mqttPass",   mqttPass);
  prefs.putString("tasmotaTpc", tasmotaTopic);
  prefs.putInt("mqttPort",      mqttPort);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("grinder", true);
  targetWeight = prefs.getFloat("targetW",    18.0f);
  preoffset    = prefs.getFloat("preoffset",   0.5f);
  grindDelay   = prefs.getFloat("grindDelay", 300.0f);
  grindCount   = prefs.getInt("grindCount",    0);
  autoTare     = prefs.getBool("autoTare",    true);
  autoRestart  = prefs.getBool("autoRestart", true);
  delayAdjust  = prefs.getBool("delayAdjust", true);
  prefs.getString("mqttServer", mqttServer,   sizeof(mqttServer));
  prefs.getString("mqttUser",   mqttUser,     sizeof(mqttUser));
  prefs.getString("mqttPass",   mqttPass,     sizeof(mqttPass));
  prefs.getString("tasmotaTpc", tasmotaTopic, sizeof(tasmotaTopic));
  mqttPort = prefs.getInt("mqttPort", 1883);
  prefs.end();
  Serial.printf("[PREFS] target=%.1f pre=%.1f delay=%.0f count=%d\n",
                targetWeight, preoffset, grindDelay, grindCount);
  Serial.printf("[PREFS] MQTT=%s:%d topic=%s\n", mqttServer, mqttPort, tasmotaTopic);
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i=0; i<len; i++) msg += (char)payload[i];
  relayOn = (msg == "ON");
  Serial.printf("[MQTT←] %s: %s\n", topic, msg.c_str());
}

void mqttReconnect() {
  if (mqttClient.connected()) return;
  Serial.printf("[MQTT] Ansluter till %s:%d...\n", mqttServer, mqttPort);
  String id = "GrinderCutoff-" + String(random(0xffff), HEX);
  bool ok = strlen(mqttUser) > 0
    ? mqttClient.connect(id.c_str(), mqttUser, mqttPass)
    : mqttClient.connect(id.c_str());
  if (ok) {
    char t[80]; snprintf(t, sizeof(t), "stat/%s/POWER", tasmotaTopic);
    mqttClient.subscribe(t);
    Serial.println("[MQTT] ✓ Ansluten!");
    Serial.printf("[MQTT] Prenumererar på: %s\n", t);
  } else {
    Serial.printf("[MQTT] ✗ Misslyckades (rc=%d)\n", mqttClient.state());
  }
}

// ─── Webbserver ───────────────────────────────────────────────────────────────
void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send_P(200,"text/html",HTML_PAGE); });
  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* r){
    const char* sn[] = {"IDLE","TARING","WAITING_STABLE","GRINDING","SETTLING","DONE"};
    char json[512];
    snprintf(json, sizeof(json),
      "{\"weight\":%.2f,\"targetWeight\":%.1f,\"preoffset\":%.1f,"
      "\"stopAt\":%.2f,\"relay\":%s,\"ble\":%s,"
      "\"state\":\"%s\",\"grindCount\":%d,\"grindDelay\":%.1f,"
      "\"lastActual\":%.2f,\"autoTare\":%s,\"autoRestart\":%s,\"delayAdjust\":%s,"
      "\"mqttServer\":\"%s\",\"mqttPort\":%d,\"tasmotaTopic\":\"%s\",\"mqttUser\":\"%s\"}",
      currentWeight, targetWeight, preoffset, stopAt(),
      relayOn?"true":"false", scalesConnected?"true":"false",
      sn[grindState], grindCount, grindDelay, lastActualWeight,
      autoTare?"true":"false", autoRestart?"true":"false", delayAdjust?"true":"false",
      mqttServer, mqttPort, tasmotaTopic, mqttUser);
    r->send(200,"application/json",json);
  });
  webServer.on("/settings", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("targetWeight",true)) targetWeight = r->getParam("targetWeight",true)->value().toFloat();
    if (r->hasParam("preoffset",true))    preoffset    = r->getParam("preoffset",true)->value().toFloat();
    if (r->hasParam("autoTare",true))     autoTare     = r->getParam("autoTare",true)->value()=="1";
    if (r->hasParam("autoRestart",true))  autoRestart  = r->getParam("autoRestart",true)->value()=="1";
    if (r->hasParam("delayAdjust",true))  delayAdjust  = r->getParam("delayAdjust",true)->value()=="1";
    savePrefs(); r->send(200,"text/plain","✓ Inställningar sparade");
  });
  webServer.on("/mqtt", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("mqttServer",true))   strlcpy(mqttServer,   r->getParam("mqttServer",true)->value().c_str(),   sizeof(mqttServer));
    if (r->hasParam("tasmotaTopic",true)) strlcpy(tasmotaTopic, r->getParam("tasmotaTopic",true)->value().c_str(), sizeof(tasmotaTopic));
    if (r->hasParam("mqttUser",true))     strlcpy(mqttUser,     r->getParam("mqttUser",true)->value().c_str(),     sizeof(mqttUser));
    if (r->hasParam("mqttPass",true))     strlcpy(mqttPass,     r->getParam("mqttPass",true)->value().c_str(),     sizeof(mqttPass));
    if (r->hasParam("mqttPort",true))     mqttPort = r->getParam("mqttPort",true)->value().toInt();
    savePrefs(); mqttClient.disconnect(); mqttClient.setServer(mqttServer, mqttPort);
    Serial.printf("[MQTT] Ny config: %s:%d topic=%s\n", mqttServer, mqttPort, tasmotaTopic);
    r->send(200,"text/plain","✓ MQTT sparat — återansluter...");
  });

webServer.on("/grind/start", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!scalesConnected) { r->send(503,"text/plain","Ingen våg ansluten"); return; }
    relayTurnOn();              // Slå på relät direkt
    enterState(STATE_GRINDING); // Gå direkt till bevakningsläge för att stänga av vid 18g
    r->send(200,"text/plain","▶ Malning startad — stängs av automatiskt vid målvikt!");
  });

  webServer.on("/tare", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!scalesConnected) { r->send(503,"text/plain","Ingen våg"); return; }
    bleTare();              // Skicka bara tara-kommandot till vågen
    enterState(STATE_IDLE); // Stanna kvar i IDLE, rör INTE relät
    r->send(200,"text/plain","⚖️ Tara skickat till vågen");
  });

  webServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest* r){
    relayTurnOn();          // Återställ relät till PÅ så kvarnen har ström som standard
    enterState(STATE_IDLE);
    r->send(200,"text/plain","↺ Återställt till utgångsläge (Relä PÅ)");
  });

  webServer.begin();
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n========================================");
  Serial.println("  GrinderCutoff startar...");
  Serial.println("========================================");

  loadPrefs();
  Serial.printf("[WiFi] Ansluter till: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.printf("[WiFi] Försök %d/20...\n", tries+1); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] ✓ Ansluten!");
    Serial.printf("[WiFi] IP: %s  Signal: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin("grindercutoff")) Serial.println("[mDNS] ✓ http://grindercutoff.local");
    else Serial.println("[mDNS] Kunde inte starta");
  } else {
    Serial.println("[WiFi] ✗ Misslyckades");
  }

  Serial.printf("[MQTT] Broker: %s:%d  Topic: cmnd/%s/POWER\n", mqttServer, mqttPort, tasmotaTopic);
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);

  setupWebServer();
  Serial.println("[Web]  ✓ Webbserver startad på http://grindercutoff.local");

  Serial.println("[BLE]  Initierar...");
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new MyScanCallbacks());
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  Serial.println("[BLE]  ✓ BLE initierat");
  startBleScan();

  enterState(STATE_IDLE);
  Serial.println("----------------------------------------");
  Serial.println("  Redo! Lägg portafiltret på vågen.");
  Serial.println("----------------------------------------");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastAttempt = 0;
      if (millis() - lastAttempt > 5000) { lastAttempt = millis(); mqttReconnect(); }
    }
    mqttClient.loop();
  }

  static bool isConnecting = false;
  static unsigned long lastScanTime = 0;
  if (!scalesConnected && !isConnecting) {
    if (deviceFound && !bleScan->isScanning()) {
      isConnecting = true;
      if (!connectToScale()) {
        Serial.println("[BLE]  Misslyckades, scannar igen...");
        deviceFound = false;
        if (foundDevice) { delete foundDevice; foundDevice = nullptr; }
        lastScanTime = millis();
      }
      isConnecting = false;
    } else if (!bleScan->isScanning() && (millis() - lastScanTime > 2000)) {
      lastScanTime = millis();
      startBleScan();
    }
  } else if (scalesConnected) {
    if (bleClient == nullptr || !bleClient->isConnected()) {
      Serial.println("[BLE]  Tappade anslutning - söker igen...");
      scalesConnected = false;
      notifyChar = nullptr; writeChar = nullptr;
      if (grindState == STATE_GRINDING) { relayOff(); enterState(STATE_IDLE); }
      deviceFound = false;
      if (foundDevice) { delete foundDevice; foundDevice = nullptr; }
      lastScanTime = millis();
    }
  }

  if (grindState == STATE_SETTLING && millis() - stateEnterTime > settleTimeout) {
    adjustDelay(currentWeight, targetWeight);
    grindCount++; savePrefs();
    Serial.printf("[DONE] Slutvikt: %.2fg (mål: %.2fg) - malning #%d klar\n",
                  currentWeight, targetWeight, grindCount);
    enterState(STATE_DONE);
  }

  delay(10);
}
