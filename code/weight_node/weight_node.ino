/*
 * ============================================================
 *  Smart Cat Litter Box — Weight Node  (Fully Wireless)
 *  Board : XIAO ESP32S3
 *  Role  : HX711 weight acquisition + ESP-NOW receive cat ID
 *          + WiFi + MQTT → push data to PC wirelessly
 *
 *  Wiring:
 *    HX711 DT  → GPIO4 (D3)
 *    HX711 SCK → GPIO5 (D4)
 *
 *  MQTT topics published:
 *    litterbox/weight  → "1234.5"       every 500 ms
 *    litterbox/visits  → "{visit JSON}" after each visit
 *    litterbox/status  → "online"       on connect (retained)
 *
 *  MQTT topic subscribed:
 *    litterbox/cmd     ← "tare"         sent from Streamlit
 *
 *  Required libraries (Arduino Library Manager):
 *    - HX711_ADC      (by Olav Kallhovd)
 *    - PubSubClient   (by Nick O'Leary)
 *    - ESP32 core     (WiFi, esp_now — built in)
 * ============================================================
 */

#include <HX711_ADC.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_now.h"
#include "esp_wifi.h"

#ifndef WIFI_FALLBACK_CHANNEL
#define WIFI_FALLBACK_CHANNEL 6  // match camera_node_3class when both fail WiFi
#endif

// ── ★ Fill in your WiFi credentials and PC's local IP ────────
const char* WIFI_SSID   = "MengqiPhone";
const char* WIFI_PASS   = "gaoxiangshibenben";
const char* MQTT_BROKER = "broker.hivemq.com";  // free public MQTT broker
//   To find your Mac's IP: System Settings → Wi-Fi → Details → IP Address
//   Or run in Terminal:  ipconfig getifaddr en0
const int   MQTT_PORT   = 1883;
// ─────────────────────────────────────────────────────────────

// ── HX711 pins ───────────────────────────────────────────────
const int HX711_dout = 4;
const int HX711_sck  = 5;
HX711_ADC LoadCell(HX711_dout, HX711_sck);
const int CAL_EEPROM_ADDR = 0;

// ── MQTT topics ───────────────────────────────────────────────
#define TOPIC_WEIGHT  "litterbox/weight"
#define TOPIC_VISITS  "litterbox/visits"
#define TOPIC_STATUS  "litterbox/status"
#define TOPIC_CMD     "litterbox/cmd"

// ── ESP-NOW packet struct (must match camera_node) ───────────
typedef struct {
  uint8_t  cat_id;
  float    confidence;
  uint32_t timestamp_ms;
  char     method[8];
} CatIDPacket;

// ── Visit detection thresholds (grams) ───────────────────────
// netWeight = reading - baseline. UI ~700–800 g spikes need enter threshold below that.
// (2000 g was for full litter+猫; too high for current scale readings.)
const float CAT_ENTER_THRESHOLD_G = 400.0;
const float CAT_EXIT_THRESHOLD_G  = 150.0;
const int   STABLE_MS             = 2000;
const int   POST_EXIT_WAIT_MS     = 3000;

// ── Weight-based cat identification ──────────────────────────
const float WESLEY_WEIGHT_G    = 4300.0;
const float PUPU_WEIGHT_G      = 4700.0;
const float WEIGHT_TOLERANCE_G = 300.0;

// ── State machine ─────────────────────────────────────────────
enum VisitState { IDLE, ENTERING, OCCUPIED, LEAVING, POST_EXIT };
VisitState visitState = IDLE;

struct VisitRecord {
  unsigned long entry_ms;
  unsigned long exit_ms;
  float  baseline_before_g;
  float  peak_weight_g;
  float  baseline_after_g;
  uint8_t cat_id;
  float   cam_confidence;
};
VisitRecord curVisit;

// ── Smart baseline — two-phase EMA with stability gating ─────
//
//  Phase 1 (fast):  first BASELINE_FAST_COUNT stable updates
//                   → large blend, baseline converges quickly after power-on
//  Phase 2 (slow):  thereafter → small blend, tracks slow litter drift only
//
//  Gate conditions (both must hold to allow an update):
//    1. std of last BASELINE_WIN readings < BASELINE_STD_G  (signal is stable)
//    2. mean is within BASELINE_NEAR_G of current baseline  (not a cat/hand)
//
const int   BASELINE_WIN        = 10;    // rolling window — larger = smoother
const int   BASELINE_MIN_N      = 4;     // min samples before first update
const int   BASELINE_FAST_COUNT = 30;    // stable updates before switching to slow phase
const float BASELINE_STD_G      = 10.0f; // stability gate (g)
const float BASELINE_NEAR_G     = 200.0f;// proximity gate (g) — cat/hand >> this
const float BASELINE_BLEND_FAST = 0.20f; // fast-phase EMA coefficient
const float BASELINE_BLEND_SLOW = 0.04f; // slow-phase EMA coefficient

float baselineWin[BASELINE_WIN];
int   baselineWinIdx      = 0;
bool  baselineWinFull     = false;
bool  baselineInitialized = false;   // avoids unsafe float == 0 comparison
int   stableUpdateCount   = 0;
float currentBaseline     = 0.0f;

void updateBaseline(float r) {
  baselineWin[baselineWinIdx] = r;
  baselineWinIdx = (baselineWinIdx + 1) % BASELINE_WIN;
  if (baselineWinIdx == 0) baselineWinFull = true;

  int n = baselineWinFull ? BASELINE_WIN : baselineWinIdx;
  if (n < BASELINE_MIN_N) return;

  float sum = 0;
  for (int i = 0; i < n; i++) sum += baselineWin[i];
  float mean = sum / n;

  float var = 0;
  for (int i = 0; i < n; i++) { float d = baselineWin[i] - mean; var += d * d; }
  float sd = sqrtf(var / n);

  if (sd >= BASELINE_STD_G) return;                                    // not stable
  if (baselineInitialized && fabsf(mean - currentBaseline) >= BASELINE_NEAR_G) return; // cat/hand on scale

  if (!baselineInitialized) {
    currentBaseline   = mean;
    baselineInitialized = true;
    stableUpdateCount   = 1;
    Serial.printf("[Baseline] Init = %.1f g\n", currentBaseline);
  } else {
    float blend = (stableUpdateCount < BASELINE_FAST_COUNT)
                  ? BASELINE_BLEND_FAST
                  : BASELINE_BLEND_SLOW;
    currentBaseline = (1.0f - blend) * currentBaseline + blend * mean;
    stableUpdateCount++;
  }
}

// ── ESP-NOW receive buffer ────────────────────────────────────
volatile bool camPktReady = false;
volatile bool camPktNotify = false;
CatIDPacket   latestCamPkt;
unsigned long camPktTime  = 0;
const int     CAM_PKT_VALID_MS = 45000;  // PIR may fire before cat steps on scale

void IRAM_ATTR onDataReceived(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len == sizeof(CatIDPacket)) {
    memcpy((void*)&latestCamPkt, data, sizeof(CatIDPacket));
    camPktTime  = millis();
    camPktReady = true;
    camPktNotify = true;
  }
}

// ── WiFi + MQTT ───────────────────────────────────────────────
WiFiClient   espClient;
PubSubClient mqtt(espClient);
unsigned long lastWeightMs = 0;
unsigned long lastReconnMs = 0;

static void wifiScanForSsid(const char* ssid) {
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("[WiFi] Scan: %d networks", n);
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      found = true;
      Serial.printf(" | \"%s\" Ch=%d RSSI=%d", ssid, WiFi.channel(i), WiFi.RSSI(i));
    }
  }
  Serial.println(found ? "" : " | hotspot NOT seen (turn on hotspot, 2.4GHz)");
  WiFi.scanDelete();
}

static bool wifiConnectStation(const char* ssid, const char* pass, int tries) {
  WiFi.setSleep(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, pass);
  Serial.printf("[WiFi] Connecting to %s", ssid);
  for (int i = 0; i < tries && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

void connectWiFi() {
  const int tries = 60;  // 30 s
  bool ok = wifiConnectStation(WIFI_SSID, WIFI_PASS, tries);
  if (!ok) {
    Serial.println("\n[WiFi] Retry once...");
    ok = wifiConnectStation(WIFI_SSID, WIFI_PASS, tries);
  }
  if (ok) {
    Serial.printf("\n[WiFi] Connected! IP=%s  Ch=%d\n",
      WiFi.localIP().toString().c_str(), WiFi.channel());
    Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WiFi] Camera ESP-NOW needs same Ch=%d\n", WiFi.channel());
  } else {
    wl_status_t st = WiFi.status();
    Serial.printf("\n[WiFi] FAILED (status=%d) — no MQTT", (int)st);
    if (st == WL_NO_SSID_AVAIL) Serial.print(" NO_SSID");
    else if (st == WL_CONNECT_FAILED) Serial.print(" AUTH_FAIL");
    else if (st == WL_DISCONNECTED) Serial.print(" DISCONNECTED");
    Serial.println();
    wifiScanForSsid(WIFI_SSID);
    esp_wifi_set_channel(WIFI_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[WiFi] ESP-NOW fallback Ch=%d (match camera WIFI_FALLBACK_CHANNEL)\n",
                  WIFI_FALLBACK_CHANNEL);
  }
}

// Called when Streamlit publishes "tare" to litterbox/cmd
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg((char*)payload, len);
  if (String(topic) == TOPIC_CMD && msg == "tare") {
    LoadCell.tareNoDelay();
    Serial.println("[MQTT] Remote tare triggered");
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  String id = "WeightNode-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connecting to %s ...\n", MQTT_BROKER);
  if (mqtt.connect(id.c_str(), nullptr, nullptr, TOPIC_STATUS, 0, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD);
    Serial.println("[MQTT] Connected");
  } else {
    Serial.printf("[MQTT] FAILED, state=%d\n", mqtt.state());
  }
}

// Publish to MQTT and also echo to serial (for debugging)
void publish(const char* topic, const String& payload, bool retain = false) {
  Serial.printf("%s:%s\n", topic, payload.c_str());
  if (mqtt.connected()) mqtt.publish(topic, payload.c_str(), retain);
}

// ─────────────────────────────────────────────────────────────
uint8_t identifyByCatWeight(float w) {
  if (fabs(w - WESLEY_WEIGHT_G) <= WEIGHT_TOLERANCE_G) return 1;
  if (fabs(w - PUPU_WEIGHT_G)   <= WEIGHT_TOLERANCE_G) return 2;
  return 0;
}

void logVisit(VisitRecord& v) {
  float dur_s       = (v.exit_ms - v.entry_ms) / 1000.0;
  float cat_w       = v.peak_weight_g - v.baseline_after_g;
  float excrement_g = max(0.0f, v.baseline_after_g - v.baseline_before_g);

  uint8_t fid  = v.cat_id;
  float   conf = v.cam_confidence;
  const char* method = "CAM";
  if (fid == 0 || conf < 0.6) {
    fid    = identifyByCatWeight(cat_w);
    conf   = (fid != 0) ? 0.85f : 0.0f;
    method = "WEIGHT";
  }
  const char* names[] = {"Unknown", "Wesley", "Pupu"};

  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"cat\":\"%s\",\"cat_id\":%d,\"method\":\"%s\","
    "\"conf\":%.2f,\"duration_s\":%.1f,"
    "\"cat_weight_g\":%.1f,\"excrement_g\":%.1f,"
    "\"entry_ms\":%lu,\"exit_ms\":%lu}",
    names[fid], fid, method, conf, dur_s, cat_w, excrement_g,
    v.entry_ms, v.exit_ms);

  // retain=true so Streamlit gets last visit on subscribe/reconnect
  publish(TOPIC_VISITS, String(buf), true);
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== Weight Node (Wireless) ===");

  // Connect WiFi first — ESP-NOW will use the same channel automatically
  WiFi.mode(WIFI_STA);
  connectWiFi();

  // ESP-NOW (channel follows WiFi)
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataReceived);
    Serial.println("[ESP-NOW] Ready");
  } else {
    Serial.println("[ESP-NOW] Init FAILED");
  }

  // MQTT
  connectMQTT();

  // HX711
  LoadCell.begin();
  float calVal;
  EEPROM.begin(512);
  EEPROM.get(CAL_EEPROM_ADDR, calVal);
  if (isnan(calVal) || fabsf(calVal) < 10.0) calVal = 696.0;
  LoadCell.start(5000, true);
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("[HX711] Timeout — check wiring"); while (1);
  }
  LoadCell.setCalFactor(calVal);
  Serial.printf("[HX711] Ready, cal=%.2f\n", calVal);
  memset(baselineWin, 0, sizeof(baselineWin));
  Serial.println("[Ready] Monitoring started.");
}

// ─────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────
void loop() {
  // Retry WiFi if hotspot was off at boot (every 30 s)
  static unsigned long lastWifiRetryMs = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetryMs > 30000) {
    lastWifiRetryMs = millis();
    Serial.println("[WiFi] Retrying...");
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) connectMQTT();
  }

  // MQTT keepalive + reconnect
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      unsigned long now = millis();
      if (now - lastReconnMs > 5000) { lastReconnMs = now; connectMQTT(); }
    }
    mqtt.loop();
  }

  if (camPktNotify) {
    camPktNotify = false;
    const char* names[] = {"Unknown", "Wesley", "Pupu"};
    uint8_t id = latestCamPkt.cat_id;
    if (id > 2) id = 0;
    Serial.printf("[ESP-NOW] RX cat=%s conf=%.2f (valid %lu s for next visit)\n",
                  names[id], latestCamPkt.confidence,
                  (unsigned long)(CAM_PKT_VALID_MS / 1000));
  }

  if (!LoadCell.update()) return;
  float reading   = LoadCell.getData();
  float netWeight = reading - currentBaseline;
  unsigned long now = millis();

  // Publish live weight every 500 ms
  if (now - lastWeightMs >= 500) {
    char buf[16]; snprintf(buf, sizeof(buf), "%.1f", reading);
    publish(TOPIC_WEIGHT, String(buf));
    lastWeightMs = now;
  }

  // Hardware tare via serial (optional, for debugging with USB)
  if (Serial.available() && Serial.read() == 't') LoadCell.tareNoDelay();
  if (LoadCell.getTareStatus()) {
    Serial.println("tare:ok");
    currentBaseline     = 0.0f;
    baselineInitialized = false;
    stableUpdateCount   = 0;
    memset(baselineWin, 0, sizeof(baselineWin));
    baselineWinIdx = 0; baselineWinFull = false;
  }

  // Expire stale camera packet
  if (camPktReady && (now - camPktTime > CAM_PKT_VALID_MS)) camPktReady = false;

  // ── Visit state machine ───────────────────────────────────
  static unsigned long stableStartMs = 0;

  switch (visitState) {
    case IDLE:
      updateBaseline(reading);
      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        visitState    = ENTERING;
        stableStartMs = now;
        curVisit      = {now, 0, currentBaseline, reading, 0, 0, 0};
        Serial.printf("[VISIT] Entering... net=%.0f g (need >%.0f)\n",
                      netWeight, CAT_ENTER_THRESHOLD_G);
      }
      break;

    case ENTERING:
      curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);
      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        if (now - stableStartMs >= (unsigned long)STABLE_MS) {
          visitState = OCCUPIED;
          if (camPktReady && (now - camPktTime < CAM_PKT_VALID_MS)) {
            curVisit.cat_id = latestCamPkt.cat_id;
            curVisit.cam_confidence = latestCamPkt.confidence;
            camPktReady = false;
            Serial.printf("[VISIT] Using camera cat_id=%d conf=%.2f\n",
                          curVisit.cat_id, curVisit.cam_confidence);
          } else {
            Serial.println("[VISIT] No recent camera packet — will use weight ID");
          }
          Serial.printf("[VISIT] Occupied — peak=%.1fg\n", curVisit.peak_weight_g);
        }
      } else { visitState = IDLE; }
      break;

    case OCCUPIED:
      curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);
      if (netWeight < CAT_EXIT_THRESHOLD_G) {
        visitState = LEAVING; curVisit.exit_ms = now; stableStartMs = now;
        Serial.println("[VISIT] Leaving...");
      }
      break;

    case LEAVING:
      if (now - stableStartMs >= (unsigned long)POST_EXIT_WAIT_MS) {
        visitState = POST_EXIT; stableStartMs = now;
      }
      break;

    case POST_EXIT:
      if (now - stableStartMs >= 2000) {
        curVisit.baseline_after_g = reading;
        logVisit(curVisit);
        for (int i = 0; i < BASELINE_WIN; i++) baselineWin[i] = reading;
        baselineWinIdx      = 0;
        baselineWinFull     = true;
        baselineInitialized = true;
        stableUpdateCount   = BASELINE_FAST_COUNT; // skip fast phase — already stable
        currentBaseline     = reading;
        visitState = IDLE;
        Serial.println("[VISIT] Logged.");
      }
      break;
  }
  delay(20);
}
