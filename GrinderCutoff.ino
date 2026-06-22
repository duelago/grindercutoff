/*
 * GrinderCutoff - ESP32-C6 Arduino Sketch
 * =========================================
 * Reads weight from MyScale KP2048B via BLE and cuts power
 * to a Tasmota plug via MQTT or local HTTP when target weight is reached.
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
#include <HTTPClient.h>

void savePrefs();
void startBleScan();
void onWeightReceived(float weight);
void relayOff();
void relayTurnOn();

// ─── Configuration ────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

char mqttServer[64]   = "192.168.1.x";
char mqttUser[32]     = "";
char mqttPass[32]     = "";
int  mqttPort         = 1883;
char tasmotaTopic[64] = "sonoff";
char tasmotaIP[64]    = "192.168.1.x";
bool localControl     = true;

float targetWeight = 18.0f;
float preoffset    = 0.5f;

// ─── State machine ────────────────────────────────────────────────────────────
enum GrindState {
  STATE_IDLE,
  STATE_GRINDING,
  STATE_SETTLING,
  STATE_DONE
};
GrindState    grindState     = STATE_IDLE;
unsigned long stateEnterTime = 0;
unsigned long settleTimeout  = 3000;

float   currentWeight   = 0.0f;
bool    relayOn         = false;
bool    scalesConnected = false;
unsigned long lastWeightTime = 0;

float lastActualWeight = 0.0f;
int   grindCount       = 0;
float grindDelay       = 300.0f;
bool  delayAdjust      = true;

// ─── BLE ──────────────────────────────────────────────────────────────────────
#define MYSCALE_SERVICE_UUID  "0000ffb0-0000-1000-8000-00805f9b34fb"
#define MYSCALE_NOTIFY_UUID   "0000ffb2-0000-1000-8000-00805f9b34fb"
#define MYSCALE_WRITE_UUID    "0000ffb1-0000-1000-8000-00805f9b34fb"
#define GRIND_SAFETY_MS       60000UL

static const uint8_t TARE_CMD[] = {
  0xAC, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xD2, 0xD2
};
Preferences    prefs;
WiFiClient     wifiClient;
PubSubClient   mqttClient(wifiClient);
AsyncWebServer webServer(80);
BLEScan* bleScan    = nullptr;
BLEClient* bleClient  = nullptr;
BLERemoteCharacteristic* notifyChar = nullptr;
BLERemoteCharacteristic* writeChar  = nullptr;
BLEAdvertisedDevice* foundDevice = nullptr;
bool deviceFound = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────
float stopAt() { return targetWeight - preoffset; }

void enterState(GrindState newState) {
  grindState     = newState;
  stateEnterTime = millis();
  const char* names[] = {"IDLE","GRINDING","SETTLING","DONE"};
  Serial.printf("[STATE] → %s\n", names[newState]);
}

// ─── Tasmota control (MQTT or local HTTP) ────────────────────────────────────
void sendTasmotaCommand(const char* cmd) {
  if (localControl) {
    if (strlen(tasmotaIP) == 0) { Serial.println("[LOCAL] No Tasmota IP configured!"); return; }
    char url[128];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Power%%20%s", tasmotaIP, cmd);
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    Serial.printf("[LOCAL] %s → HTTP %d\n", url, code);
    http.end();
  } else {
    if (!mqttClient.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "cmnd/%s/POWER", tasmotaTopic);
    mqttClient.publish(topic, cmd);
    Serial.printf("[MQTT→] %s: %s\n", topic, cmd);
  }
}

bool fetchLocalRelayState() {
  if (strlen(tasmotaIP) == 0) return false;
  char url[128];
  snprintf(url, sizeof(url), "http://%s/cm?cmnd=Power", tasmotaIP);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(500);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    bool isOn = body.indexOf("\"ON\"") >= 0;
    http.end();
    return isOn;
  }
  http.end();
  return relayOn;
}

void relayOff() {
  sendTasmotaCommand("OFF");
  relayOn = false;
  Serial.println("[RELAY] OFF");
}

void relayTurnOn() {
  sendTasmotaCommand("ON");
  relayOn = true;
  Serial.println("[RELAY] ON");
}

// ─── Weight handling ──────────────────────────────────────────────────────────
void onWeightReceived(float weight) {
  currentWeight  = weight;
  lastWeightTime = millis();
  switch (grindState) {
    case STATE_IDLE:
      break;
    case STATE_GRINDING:
      Serial.printf("[GBW] %.3fg / %.1fg (stop at %.1fg)\n", weight, targetWeight, stopAt());
      if (weight >= stopAt()) {
        relayOff();
        lastActualWeight = weight;
        enterState(STATE_SETTLING);
      }
      break;
    case STATE_SETTLING:
      break;
    case STATE_DONE:
      if (weight < -1.0f) {
        Serial.println("[AUTO] Portafilter removed - ready for next grind");
        enterState(STATE_IDLE);
      }
      break;
  }
}

// ─── BLE: weight parsing ──────────────────────────────────────────────────────
void notifyCallback(BLERemoteCharacteristic* c, uint8_t* data, size_t length, bool isNotify) {
  if (length < 15) return;
  bool isNegative = ((data[2] >> 4) == 0x8 || (data[2] >> 4) == 0xC);
  uint32_t raw =
    ((uint32_t)(data[3] & 0x0F) << 24) |
    ((uint32_t)data[4] << 16) |
    ((uint32_t)data[5] <<  8) |
    ((uint32_t)data[6]);
  float weight = (float)(isNegative ? -(int32_t)raw : (int32_t)raw) / 1000.0f;
  onWeightReceived(weight);
}

// ─── BLE: tare ───────────────────────────────────────────────────────────────
void bleTare() {
  if (writeChar && scalesConnected) {
    writeChar->writeValue((uint8_t*)TARE_CMD, sizeof(TARE_CMD), false);
    Serial.println("[BLE]  Tare sent");
  }
}

// ─── BLE: scan ───────────────────────────────────────────────────────────────
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

// ─── BLE: connect ─────────────────────────────────────────────────────────────
bool connectToScale() {
  if (!foundDevice) return false;
  Serial.printf("[BLE]  Connecting to %s...\n", foundDevice->getAddress().toString().c_str());
  if (bleClient) { delete bleClient; bleClient = nullptr; }
  bleClient = BLEDevice::createClient();
  if (!bleClient->connect(foundDevice)) { Serial.println("[BLE]  ✗ Failed"); return false; }
  Serial.println("[BLE]  ✓ Connected, fetching services...");
  delay(200);
  BLERemoteService* svc = bleClient->getService(MYSCALE_SERVICE_UUID);
  if (!svc) { Serial.println("[BLE]  ✗ Service FFB0 not found"); bleClient->disconnect(); return false; }
  notifyChar = svc->getCharacteristic(MYSCALE_NOTIFY_UUID);
  writeChar  = svc->getCharacteristic(MYSCALE_WRITE_UUID);
  if (!notifyChar || !writeChar) { Serial.println("[BLE]  ✗ Characteristic not found"); bleClient->disconnect(); return false; }
  if (!notifyChar->canNotify()) { Serial.println("[BLE]  ✗ Notify not supported"); bleClient->disconnect(); return false; }
  notifyChar->registerForNotify(notifyCallback, true);
  Serial.println("[BLE]  ✓ Notifications registered");
  delay(300);
  scalesConnected = true;
  Serial.println("[BLE]  ✓ Ready!");
  return true;
}

// ─── BLE: scan callback ───────────────────────────────────────────────────────
class MyScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) {
    bool found = false;
    if (device.haveName()) {
      String name = device.getName().c_str();
      if (name == "MY_SCALE" || name == "my_scale" || name == "blackcoffee") {
        Serial.printf("\n[BLE]  Found: \"%s\"\n", name.c_str());
        found = true;
      }
    }
    if (!found && device.haveServiceUUID() && device.isAdvertisingService(BLEUUID(MYSCALE_SERVICE_UUID))) {
      Serial.println("\n[BLE]  Found by service UUID");
      found = true;
    }
    if (!found) {
      String mac = String(device.getAddress().toString().c_str());
      mac.toLowerCase();
      if (mac == "d0:4d:00:6e:2a:91") { Serial.println("\n[BLE]  Found by MAC"); found = true; }
    }
    if (found && !deviceFound) {
      bleScan->stop();
      if (foundDevice) delete foundDevice;
      foundDevice = new BLEAdvertisedDevice(device);
      deviceFound = true;
    }
  }
};

// ─── Adaptive delay ───────────────────────────────────────────────────────────
void adjustDelay(float actual, float target) {
  if (!delayAdjust || grindCount < 2) return;
  float error = actual - target;
  float adj   = constrain(error * 50.0f, -500.0f, 500.0f);
  grindDelay  = constrain(grindDelay + adj, 0.0f, 3000.0f);
  Serial.printf("[ADAPT] Actual=%.2fg Target=%.2fg → delay=%.0fms\n", actual, target, grindDelay);
  prefs.begin("grinder", false); prefs.putFloat("grindDelay", grindDelay); prefs.end();
}

// ─── Save / load ──────────────────────────────────────────────────────────────
void savePrefs() {
  prefs.begin("grinder", false);
  prefs.putFloat("targetW",    targetWeight);
  prefs.putFloat("preoffset",  preoffset);
  prefs.putFloat("grindDelay", grindDelay);
  prefs.putInt("grindCount",   grindCount);
  prefs.putBool("delayAdjust", delayAdjust);
  prefs.putString("mqttServer", mqttServer);
  prefs.putString("mqttUser",   mqttUser);
  prefs.putString("mqttPass",   mqttPass);
  prefs.putString("tasmotaTpc", tasmotaTopic);
  prefs.putInt("mqttPort",      mqttPort);
  prefs.putString("tasmotaIP",  tasmotaIP);
  prefs.putBool("localCtrl",    localControl);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("grinder", true);
  targetWeight = prefs.getFloat("targetW",    18.0f);
  preoffset    = prefs.getFloat("preoffset",   0.5f);
  grindDelay   = prefs.getFloat("grindDelay", 300.0f);
  grindCount   = prefs.getInt("grindCount",    0);
  delayAdjust  = prefs.getBool("delayAdjust", true);
  prefs.getString("mqttServer", mqttServer,   sizeof(mqttServer));
  prefs.getString("mqttUser",   mqttUser,     sizeof(mqttUser));
  prefs.getString("mqttPass",   mqttPass,     sizeof(mqttPass));
  prefs.getString("tasmotaTpc", tasmotaTopic, sizeof(tasmotaTopic));
  mqttPort     = prefs.getInt("mqttPort", 1883);
  prefs.getString("tasmotaIP",  tasmotaIP,    sizeof(tasmotaIP));
  localControl = prefs.getBool("localCtrl",   false);
  prefs.end();
  Serial.printf("[PREFS] target=%.1f pre=%.1f count=%d\n", targetWeight, preoffset, grindCount);
  Serial.printf("[PREFS] Mode: %s\n", localControl ? "LOCAL HTTP" : "MQTT");
  if (localControl) Serial.printf("[PREFS] Tasmota IP: %s\n", tasmotaIP);
  else Serial.printf("[PREFS] MQTT=%s:%d topic=%s\n", mqttServer, mqttPort, tasmotaTopic);
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (localControl) return; // Säkerhetsspärr
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT←] %s: %s\n", topic, msg.c_str());
  String statTopic = String("stat/") + tasmotaTopic + "/POWER";
  if (String(topic) == statTopic) {
    if (msg == "ON" && !relayOn) {
      relayOn = true;
      if (grindState == STATE_IDLE && scalesConnected) {
        Serial.println("[AUTO] Relay ON detected → starting weight monitoring");
        enterState(STATE_GRINDING);
      }
    } else if (msg == "OFF") {
      relayOn = false;
      if (grindState == STATE_GRINDING) {
        Serial.println("[AUTO] Relay OFF externally during grind → DONE");
        lastActualWeight = currentWeight;
        enterState(STATE_DONE);
      }
    }
  }
}

void mqttReconnect() {
  if (localControl || mqttClient.connected()) return; // Förhindra anslutning om lokal styrning körs
  Serial.printf("[MQTT] Connecting to %s:%d...\n", mqttServer, mqttPort);
  String id = "GrinderCutoff-" + String(random(0xffff), HEX);
  bool ok = strlen(mqttUser) > 0
    ? mqttClient.connect(id.c_str(), mqttUser, mqttPass)
    : mqttClient.connect(id.c_str());
  if (ok) {
    char t[80];
    snprintf(t, sizeof(t), "stat/%s/POWER", tasmotaTopic);
    mqttClient.subscribe(t);
    Serial.printf("[MQTT] ✓ Connected! Subscribing to: %s\n", t);
    static bool bootOffSent = false;
    if (!bootOffSent) {
      Serial.println("[SAFETY] Boot - sending OFF to Tasmota (MQTT)");
      sendTasmotaCommand("OFF");
      bootOffSent = true;
    }
  } else {
    Serial.printf("[MQTT] ✗ Failed (rc=%d)\n", mqttClient.state());
  }
}

// ─── HTML ─────────────────────────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
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
    .state-badge{font-size:.85em;padding:5px 16px;border-radius:20px;display:inline-block;margin:6px auto}
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
    .pw{background:#0d1b2a;border-radius:8px;height:16px;overflow:hidden;margin-top:8px}
    .pb{height:100%;background:linear-gradient(90deg,#4ecca3,#e94560);transition:width .5s;border-radius:8px}
    .msg{color:#4ecca3;font-size:.85em;text-align:center;min-height:18px;margin-top:4px}
    .stat-table{width:100%;font-size:.85em;border-collapse:collapse}
    .stat-table td{padding:5px 0;color:#aaa}.stat-table td:last-child{color:#fff;text-align:right}
    .section-label{font-size:.75em;font-weight:bold;letter-spacing:1px;color:#e94560;
         text-transform:uppercase;margin:14px 0 10px;padding-top:14px;
         border-top:1px solid #0f3460}
    .active-mode{border:1px solid #4ecca3 !important;border-radius:8px;padding:12px;margin-bottom:4px}
    .inactive-mode{border:1px solid #2a2a4a;border-radius:8px;padding:12px;margin-bottom:4px;opacity:.55}
  </style>
</head>
<body>
  <h1>☕ GrinderCutoff</h1>
  <p class="sub">MyScale KP2048B + Tasmota</p>

  <div class="card">
    <h2>Live</h2>
    <div class="big" id="weightVal">-.-</div>
    <div style="text-align:center">
      <span class="state-badge" id="stateBadge" style="background:#333">–</span>
    </div>
    <div class="pw"><div class="pb" id="progressBar" style="width:0%"></div></div>
    <div class="status-row">
      <span class="badge ble-err" id="bleBadge">BLE: searching...</span>
      <span class="badge rel-off" id="relayBadge">Relay: OFF</span>
    </div>
  </div>

  <div class="card">
    <h2>Statistics</h2>
    <table class="stat-table">
      <tr><td>Grind count</td><td id="grindCount">0</td></tr>
      <tr><td>Last final weight</td><td id="lastActual">–</td></tr>
      <tr><td>Stop weight (target − offset)</td><td id="stopAtVal">–</td></tr>
      <tr><td>Learned delay</td><td id="grindDelay">–</td></tr>
    </table>
  </div>

  <div class="card">
    <h2>Settings</h2>
    <div class="row">
      <div><label>Target weight (g)</label>
        <input type="number" id="targetWeight" step="0.1" min="1" value="18.0"></div>
      <div><label>Pre-offset (g) 🔧</label>
        <input type="number" id="preoffset" step="0.1" min="0" value="0.5"></div>
    </div>
    <div class="toggle-row"><label>Adaptive delay learning</label>
      <input type="checkbox" id="delayAdjust" checked></div>
    <button class="btn-save" onclick="saveSettings()">💾 Save settings</button>
    <div class="msg" id="saveMsg"></div>
  </div>

  <div class="card">
    <h2>Tasmota connection</h2>

    <div class="section-label">Option A — MQTT</div>
    <div id="mqttBox">
      <div class="toggle-row">
        <label>Use MQTT</label>
        <input type="radio" name="ctrlMode" id="useMQTT" value="mqtt" onchange="modeChanged()">
      </div>
      <label>MQTT server (IP)</label>
      <input type="text" id="mqttServer" placeholder="192.168.1.X">
      <div class="row">
        <div><label>Port</label>
          <input type="number" id="mqttPort" value="1883" min="1" max="65535"></div>
        <div><label>Topic</label>
          <input type="text" id="tasmotaTopic" placeholder="sonoff"></div>
      </div>
      <div class="row">
        <div><label>Username</label>
          <input type="text" id="mqttUser" placeholder="(empty = no auth)"></div>
        <div><label>Password</label>
          <input type="password" id="mqttPass" placeholder="(empty = no auth)"></div>
      </div>
    </div>

    <div class="section-label">Option B — Local HTTP (no broker needed)</div>
    <div id="localBox">
      <div class="toggle-row">
        <label>Use local HTTP</label>
        <input type="radio" name="ctrlMode" id="useLocal" value="local" onchange="modeChanged()">
      </div>
      <label>Tasmota IP address</label>
      <input type="text" id="tasmotaIP" placeholder="192.168.1.X">
    </div>

    <button class="btn-save" onclick="saveConnection()">💾 Save connection</button>
    <div class="msg" id="mqttMsg"></div>
  </div>

  <div class="card">
    <h2>Manual control</h2>
    <button class="btn-relay" onclick="toggleRelay()">⚡ Toggle relay</button>
    <button class="btn-tare"  onclick="tare()">⚖️ Tare scale (via web)</button>
    <div class="msg" id="ctrlMsg"></div>
  </div>

<script>
  const STATES = {
    'IDLE':'⏸ IDLE','GRINDING':'🟢 GRINDING','SETTLING':'🟠 SETTLING','DONE':'✅ DONE'
  };
  const STATE_COLORS = {
    'IDLE':'#444','GRINDING':'#2a7a4b','SETTLING':'#7a5500','DONE':'#0f5c3a'
  };
  
  const connFields = ['mqttServer','mqttPort','tasmotaTopic','mqttUser','mqttPass','tasmotaIP', 'useMQTT', 'useLocal'];
  
  function connFocused() { 
    return connFields.includes(document.activeElement.id);
  }

  function modeChanged() {
    const local = document.getElementById('useLocal').checked;
    document.getElementById('mqttBox').className   = local ? 'inactive-mode' : 'active-mode';
    document.getElementById('localBox').className  = local ? 'active-mode'   : 'inactive-mode';
  }

  function fetchStatus() {
    fetch('/status').then(r=>r.json()).then(d=>{
      document.getElementById('weightVal').textContent = d.weight.toFixed(1)+' g';
      const pct = Math.min(100, Math.max(0, d.weight / Math.max(0.01, d.stopAt) * 100));
      document.getElementById('progressBar').style.width = pct+'%';
      document.getElementById('bleBadge').textContent  = d.ble ? 'BLE: ✓' : 'BLE: searching...';
      document.getElementById('bleBadge').className    = 'badge '+(d.ble?'ble-ok':'ble-err');
      document.getElementById('relayBadge').textContent= d.relay ? 'Relay: ON' : 'Relay: OFF';
      document.getElementById('relayBadge').className  = 'badge '+(d.relay?'rel-on':'rel-off');
      const sb = document.getElementById('stateBadge');
      sb.textContent = STATES[d.state] || d.state;
      sb.style.background = STATE_COLORS[d.state] || '#333';
      document.getElementById('grindCount').textContent = d.grindCount;
      document.getElementById('lastActual').textContent = d.lastActual > 0 ? d.lastActual.toFixed(1)+' g' : '–';
      document.getElementById('stopAtVal').textContent  = d.stopAt.toFixed(1)+' g';
      document.getElementById('grindDelay').textContent = d.grindDelay.toFixed(0)+' ms';
      document.getElementById('targetWeight').value = d.targetWeight;
      document.getElementById('preoffset').value    = d.preoffset;
      document.getElementById('delayAdjust').checked= d.delayAdjust;
      
      if (!connFocused()) {
        document.getElementById('useMQTT').checked  = !d.localControl;
        document.getElementById('useLocal').checked =  d.localControl;
        document.getElementById('mqttServer').value    = d.mqttServer;
        document.getElementById('mqttPort').value      = d.mqttPort;
        document.getElementById('tasmotaTopic').value  = d.tasmotaTopic;
        document.getElementById('mqttUser').value      = d.mqttUser;
        document.getElementById('tasmotaIP').value     = d.tasmotaIP || '';
        modeChanged();
      }
    }).catch(()=>{});
  }

  function saveSettings(){
    const b = new URLSearchParams({
      targetWeight: document.getElementById('targetWeight').value,
      preoffset:    document.getElementById('preoffset').value,
      delayAdjust:  document.getElementById('delayAdjust').checked?'1':'0'
    });
    fetch('/settings',{method:'POST',body:b}).then(r=>r.text()).then(t=>{
      document.getElementById('saveMsg').textContent=t;
      setTimeout(()=>document.getElementById('saveMsg').textContent='',3000);
    });
  }

  function saveConnection(){
    const local = document.getElementById('useLocal').checked;
    const b = new URLSearchParams({
      localControl: local?'1':'0',
      tasmotaIP:    document.getElementById('tasmotaIP').value,
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

  function toggleRelay(){api('/relay/toggle','ctrlMsg');}
  function tare(){api('/tare','ctrlMsg');}
  function api(url,msgId){
    fetch(url).then(r=>r.text()).then(t=>{
      document.getElementById(msgId).textContent=t;
      setTimeout(()=>document.getElementById(msgId).textContent='',3000);
    });
  }

  fetchStatus();
  setInterval(fetchStatus,500);
</script>
</body>
</html>
)rawliteral";

// ─── Web server ───────────────────────────────────────────────────────────────
void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send_P(200,"text/html",HTML_PAGE);
  });

  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* r){
    const char* sn[] = {"IDLE","GRINDING","SETTLING","DONE"};
    char json[512];
    snprintf(json, sizeof(json),
      "{\"weight\":%.3f,\"targetWeight\":%.1f,\"preoffset\":%.1f,"
      "\"stopAt\":%.1f,\"relay\":%s,\"ble\":%s,"
      "\"state\":\"%s\",\"grindCount\":%d,\"grindDelay\":%.1f,"
      "\"lastActual\":%.3f,\"delayAdjust\":%s,"
      "\"localControl\":%s,\"tasmotaIP\":\"%s\","
      "\"mqttServer\":\"%s\",\"mqttPort\":%d,\"tasmotaTopic\":\"%s\",\"mqttUser\":\"%s\"}",
      currentWeight, targetWeight, preoffset, stopAt(),
      relayOn?"true":"false", scalesConnected?"true":"false",
      sn[grindState], grindCount, grindDelay, lastActualWeight,
      delayAdjust?"true":"false",
      localControl?"true":"false", tasmotaIP,
      mqttServer, mqttPort, tasmotaTopic, mqttUser);
    r->send(200,"application/json",json);
  });

  webServer.on("/settings", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("targetWeight",true)) targetWeight = r->getParam("targetWeight",true)->value().toFloat();
    if (r->hasParam("preoffset",true))    preoffset    = r->getParam("preoffset",true)->value().toFloat();
    if (r->hasParam("delayAdjust",true))  delayAdjust  = r->getParam("delayAdjust",true)->value()=="1";
    savePrefs();
    r->send(200,"text/plain","✓ Settings saved");
  });
  
  webServer.on("/mqtt", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("localControl",true)) localControl  = r->getParam("localControl",true)->value()=="1";
    if (r->hasParam("tasmotaIP",true))    strlcpy(tasmotaIP,    r->getParam("tasmotaIP",true)->value().c_str(),    sizeof(tasmotaIP));
    if (r->hasParam("mqttServer",true))   strlcpy(mqttServer,   r->getParam("mqttServer",true)->value().c_str(),   sizeof(mqttServer));
    if (r->hasParam("tasmotaTopic",true)) strlcpy(tasmotaTopic, r->getParam("tasmotaTopic",true)->value().c_str(), sizeof(tasmotaTopic));
    if (r->hasParam("mqttUser",true))     strlcpy(mqttUser,     r->getParam("mqttUser",true)->value().c_str(),     sizeof(mqttUser));
    if (r->hasParam("mqttPass",true))     strlcpy(mqttPass,     r->getParam("mqttPass",true)->value().c_str(),     sizeof(mqttPass));
    if (r->hasParam("mqttPort",true))     mqttPort = r->getParam("mqttPort",true)->value().toInt();
    
    savePrefs();
    
    // FIX: Om vi växlar till lokal kontroll, tvinga fram en urkoppling från MQTT direkt!
    if (localControl) {
      mqttClient.disconnect();
      Serial.println("[CFG] Switched to LOCAL HTTP. Disconnecting MQTT client.");
    } else {
      mqttClient.disconnect();
      mqttClient.setServer(mqttServer, mqttPort);
    }
    
    Serial.printf("[CFG] Mode: %s\n", localControl ? "LOCAL HTTP" : "MQTT");
    r->send(200,"text/plain", localControl ? "✓ Local control enabled" : "✓ MQTT saved — reconnecting...");
  });
  
  webServer.on("/relay/toggle", HTTP_GET, [](AsyncWebServerRequest* r){
    if (relayOn) {
      relayOff();
      if (grindState == STATE_GRINDING) enterState(STATE_IDLE);
      r->send(200,"text/plain","Relay: OFF");
    } else {
      relayTurnOn();
      if (grindState == STATE_IDLE && scalesConnected) enterState(STATE_GRINDING);
      r->send(200,"text/plain","Relay: ON");
    }
  });
  webServer.on("/tare", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!scalesConnected) { r->send(503,"text/plain","No scale connected"); return; }
    bleTare();
    r->send(200,"text/plain","⚖️ Tare sent");
  });
  webServer.begin();
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n========================================");
  Serial.println("  GrinderCutoff starting...");
  Serial.println("========================================");

  loadPrefs();
  Serial.printf("[WiFi] Connecting to: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.printf("[WiFi] Attempt %d/20...\n", tries+1); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] ✓ IP: %s  Signal: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin("grindercutoff")) Serial.println("[mDNS] ✓ http://grindercutoff.local");
    if (localControl && strlen(tasmotaIP) > 0) {
      Serial.println("[SAFETY] Boot - sending OFF to Tasmota (local HTTP)");
      sendTasmotaCommand("OFF");
    }
  } else {
    Serial.println("[WiFi] ✗ Failed");
  }

  // FIX: Sätt bara upp server-parametrar om vi faktiskt ska använda MQTT vid boot
  if (!localControl) {
    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);
    Serial.printf("[MQTT] Broker: %s:%d  Topic: cmnd/%s/POWER\n", mqttServer, mqttPort, tasmotaTopic);
  } else {
    Serial.printf("[LOCAL] Tasmota IP: %s\n", tasmotaIP);
  }

  setupWebServer();
  Serial.println("[Web]  ✓ http://grindercutoff.local");

  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new MyScanCallbacks());
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);
  startBleScan();

  enterState(STATE_IDLE);
  Serial.println("----------------------------------------");
  Serial.println("  Ready! Tare the scale and press the plug button.");
  Serial.println("----------------------------------------");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    // FIX: Dubbelkolla localControl extra noga här för att förhindra oväntade mqttReconnect
    if (!localControl) {
      if (!mqttClient.connected()) {
        static unsigned long lastAttempt = 0;
        if (millis() - lastAttempt > 5000) { 
          lastAttempt = millis(); 
          mqttReconnect();
        }
      } else {
        mqttClient.loop();
      }
    } else {
      static unsigned long lastPoll = 0;
      if (millis() - lastPoll > 500) {
        lastPoll = millis();
        bool newState = fetchLocalRelayState();
        if (newState && !relayOn) {
          relayOn = true;
          if (grindState == STATE_IDLE && scalesConnected) {
            Serial.println("[LOCAL] Relay ON detected → starting weight monitoring");
            enterState(STATE_GRINDING);
          }
        } else if (!newState && relayOn) {
          relayOn = false;
          if (grindState == STATE_GRINDING) {
            Serial.println("[LOCAL] Relay OFF externally → DONE");
            lastActualWeight = currentWeight;
            enterState(STATE_DONE);
          }
        }
      }
    }
  }

  static bool isConnecting = false;
  static unsigned long lastScanTime = 0;
  if (!scalesConnected && !isConnecting) {
    if (deviceFound && !bleScan->isScanning()) {
      isConnecting = true;
      if (!connectToScale()) {
        deviceFound = false;
        if (foundDevice) { delete foundDevice; foundDevice = nullptr; }
        lastScanTime = millis();
      }
      isConnecting = false;
    } else if (!bleScan->isScanning() && millis() - lastScanTime > 2000) {
      lastScanTime = millis();
      startBleScan();
    }
  } else if (scalesConnected) {
    if (!bleClient || !bleClient->isConnected()) {
      Serial.println("[BLE]  Lost connection - scanning again...");
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
    grindCount++;
    savePrefs();
    Serial.printf("[DONE] Final weight: %.3fg (target: %.1fg) - grind #%d\n",
                  currentWeight, targetWeight, grindCount);
    enterState(STATE_DONE);
  }

  if (grindState == STATE_GRINDING && millis() - stateEnterTime > GRIND_SAFETY_MS) {
    Serial.println("[SAFETY] ⚠ Grind exceeded 60s - forcing relay OFF!");
    relayOff();
    lastActualWeight = currentWeight;
    enterState(STATE_DONE);
  }

  delay(10);
}
