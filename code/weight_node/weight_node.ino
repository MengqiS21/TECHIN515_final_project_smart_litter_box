/*
 * ============================================================
 *  Smart Cat Litter Box — Weight Node
 *  Board : XIAO ESP32S3 (no camera, connected to PC via USB)
 *  Role  : HX711 weight acquisition + ESP-NOW receive cat ID + visit logging
 *
 *  Wiring:
 *    HX711 DT  → GPIO4 (D3)
 *    HX711 SCK → GPIO5 (D4)
 *
 *  Serial output format (read by Streamlit):
 *    Live weight : "val:1234.5"
 *    Visit record: "visit:{JSON}"
 *    Tare done   : "tare:ok"
 *
 *  Serial commands (sent from Streamlit):
 *    't' → tare (zero the scale)
 *
 *  Arduino IDE settings:
 *    Board: XIAO_ESP32S3 | 115200 baud
 *
 *  Required libraries:
 *    - HX711_ADC (by Olav Kallhovd)
 *    - ESP32 Arduino core (esp_now and WiFi are built in)
 * ============================================================
 */

#include <HX711_ADC.h>
#include <EEPROM.h>
#include "esp_now.h"
#include "WiFi.h"

// ── HX711 pins ───────────────────────────────────────────────
const int HX711_dout = 4;  // DT  → GPIO4
const int HX711_sck  = 5;  // SCK → GPIO5

HX711_ADC LoadCell(HX711_dout, HX711_sck);
const int CAL_EEPROM_ADDR = 0;

// ── ESP-NOW packet struct (must match camera_node) ───────────
typedef struct {
  uint8_t  cat_id;
  float    confidence;
  uint32_t timestamp_ms;
  char     method[8];
} CatIDPacket;

// ── Visit detection thresholds (grams) ───────────────────────
//   CAT_ENTER : net weight above baseline exceeds this → cat entered
//   CAT_EXIT  : net weight drops below this → cat left
//   STABLE_MS : how long the reading must be stable to confirm cat is inside
const float CAT_ENTER_THRESHOLD_G = 2000.0;  // cat weighs at least 2 kg
const float CAT_EXIT_THRESHOLD_G  = 300.0;   // less than 300 g above baseline = empty
const int   STABLE_MS             = 2000;    // 2 s stability window
const int   POST_EXIT_WAIT_MS     = 3000;    // wait 3 s after exit for litter to settle

// ── Weight-based cat identification ──────────────────────────
//   Wesley: ~4300 g,  Pupu: ~4700 g,  tolerance: ±300 g
const float WESLEY_WEIGHT_G    = 4300.0;
const float PUPU_WEIGHT_G      = 4700.0;
const float WEIGHT_TOLERANCE_G = 300.0;

// ── State machine ─────────────────────────────────────────────
enum VisitState { IDLE, ENTERING, OCCUPIED, LEAVING, POST_EXIT };
VisitState visitState = IDLE;

// ── Visit record ──────────────────────────────────────────────
struct VisitRecord {
  unsigned long entry_ms;
  unsigned long exit_ms;
  float  baseline_before_g;  // box weight before the cat entered
  float  peak_weight_g;      // highest reading while cat was inside
  float  baseline_after_g;   // stable box weight after the cat left
  uint8_t cat_id;            // 0=Unknown  1=Wesley  2=Pupu
  float   cam_confidence;    // camera confidence (0 = no packet received)
};
VisitRecord curVisit;

// ── Baseline tracking (rolling mean of last N stable readings) ─
const int BASELINE_SAMPLES = 20;
float     baselineHistory[BASELINE_SAMPLES];
int       baselineIdx  = 0;
bool      baselineFull = false;
float     currentBaseline = 0.0;

void updateBaseline(float reading) {
  baselineHistory[baselineIdx] = reading;
  baselineIdx = (baselineIdx + 1) % BASELINE_SAMPLES;
  if (baselineIdx == 0) baselineFull = true;

  int n = baselineFull ? BASELINE_SAMPLES : baselineIdx;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += baselineHistory[i];
  currentBaseline = sum / n;
}

// ── ESP-NOW receive buffer ────────────────────────────────────
volatile bool camPktReady = false;
CatIDPacket   latestCamPkt;
unsigned long camPktTime  = 0;
const int     CAM_PKT_VALID_MS = 10000;  // camera packet is valid for 10 s

void IRAM_ATTR onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
  if (len == sizeof(CatIDPacket)) {
    memcpy((void*)&latestCamPkt, data, sizeof(CatIDPacket));
    camPktTime  = millis();
    camPktReady = true;
  }
}

// Serial output every 500 ms
unsigned long lastSerialMs = 0;

// ─────────────────────────────────────────────────────────────
//  Weight-based cat ID (fallback when no camera packet)
// ─────────────────────────────────────────────────────────────
uint8_t identifyByCatWeight(float cat_weight_g) {
  if (fabs(cat_weight_g - WESLEY_WEIGHT_G) <= WEIGHT_TOLERANCE_G) return 1;  // Wesley
  if (fabs(cat_weight_g - PUPU_WEIGHT_G)   <= WEIGHT_TOLERANCE_G) return 2;  // Pupu
  return 0;  // Unknown
}

// ─────────────────────────────────────────────────────────────
//  End of visit: compute metrics and output JSON via serial
// ─────────────────────────────────────────────────────────────
void logVisit(VisitRecord& v) {
  float duration_s  = (v.exit_ms - v.entry_ms) / 1000.0;
  float cat_weight_g = v.peak_weight_g - v.baseline_after_g;
  float excrement_g  = v.baseline_after_g - v.baseline_before_g;
  if (excrement_g < 0) excrement_g = 0;  // clamp measurement error

  // Fuse camera ID and weight-based ID
  uint8_t final_cat_id = v.cat_id;
  float   final_conf   = v.cam_confidence;
  String  id_method    = "CAM";

  if (final_cat_id == 0 || final_conf < 0.6) {
    // No camera data or confidence too low — fall back to weight
    final_cat_id = identifyByCatWeight(cat_weight_g);
    final_conf   = (final_cat_id != 0) ? 0.85 : 0.0;
    id_method    = "WEIGHT";
  }

  const char* cat_names[] = {"Unknown", "Wesley", "Pupu"};

  // JSON line parsed by health_analyzer.py in Streamlit
  Serial.printf(
    "visit:{\"cat\":\"%s\",\"cat_id\":%d,\"method\":\"%s\","
    "\"conf\":%.2f,\"duration_s\":%.1f,"
    "\"cat_weight_g\":%.1f,\"excrement_g\":%.1f,"
    "\"entry_ms\":%lu,\"exit_ms\":%lu}\n",
    cat_names[final_cat_id],
    final_cat_id,
    id_method.c_str(),
    final_conf,
    duration_s,
    cat_weight_g,
    excrement_g,
    v.entry_ms,
    v.exit_ms
  );
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("=== Weight Node (ESP-NOW Receiver) ===");
  Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());
  Serial.println("[INFO] Copy the MAC above into weightNodeMAC[] in camera_node");

  // ── HX711 init ────────────────────────────────────────────
  LoadCell.begin();
  float calVal;
  EEPROM.begin(512);
  EEPROM.get(CAL_EEPROM_ADDR, calVal);
  if (isnan(calVal) || calVal == 0) {
    calVal = 696.0;  // default if not yet calibrated (run Calibration_ESP32S3 first)
    Serial.println("[HX711] WARNING: no calibration value found, using default 696.0");
  }

  LoadCell.start(5000, true);  // 5 s stabilization + auto tare
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("[HX711] Timeout! Check wiring.");
    while (1) delay(100);
  }
  LoadCell.setCalFactor(calVal);
  Serial.printf("[HX711] Ready, cal=%.2f\n", calVal);

  // ── ESP-NOW init ──────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED");
  } else {
    esp_now_register_recv_cb(onDataReceived);
    Serial.println("[ESP-NOW] Ready, listening...");
  }

  memset(baselineHistory, 0, sizeof(baselineHistory));
  Serial.println("[Ready] Monitoring started.\n");
}

// ─────────────────────────────────────────────────────────────
//  Loop — main state machine
// ─────────────────────────────────────────────────────────────
void loop() {
  // ── HX711 read ───────────────────────────────────────────
  bool newData = LoadCell.update();
  float reading = 0;
  if (newData) reading = LoadCell.getData();
  else return;  // no new data this cycle

  float netWeight = reading - currentBaseline;  // net weight above baseline
  unsigned long now = millis();

  // ── Live serial stream for Streamlit chart (every 500 ms) ─
  if (now - lastSerialMs >= 500) {
    Serial.printf("val:%.1f\n", reading);
    lastSerialMs = now;
  }

  // ── Serial command handler ────────────────────────────────
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 't') {
      LoadCell.tareNoDelay();
    }
  }
  if (LoadCell.getTareStatus()) {
    Serial.println("tare:ok");
    currentBaseline = 0;
    baselineFull    = false;
    baselineIdx     = 0;
  }

  // ── Expire stale camera packet ────────────────────────────
  if (camPktReady && (now - camPktTime > CAM_PKT_VALID_MS)) {
    camPktReady = false;
  }

  // ── Visit state machine ───────────────────────────────────
  static unsigned long stableStartMs = 0;
  static float         stablePeakG   = 0;

  switch (visitState) {

    // ── IDLE: track baseline ──────────────────────────────
    case IDLE:
      updateBaseline(reading);

      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        visitState    = ENTERING;
        stableStartMs = now;
        stablePeakG   = netWeight;
        curVisit.entry_ms          = now;
        curVisit.baseline_before_g = currentBaseline;
        curVisit.peak_weight_g     = reading;
        curVisit.cat_id            = 0;
        curVisit.cam_confidence    = 0;
        Serial.println("[VISIT] Cat entering...");
      }
      break;

    // ── ENTERING: wait for weight to stabilize ────────────
    case ENTERING:
      if (netWeight > stablePeakG) stablePeakG = netWeight;
      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);
        if (now - stableStartMs >= (unsigned long)STABLE_MS) {
          // Weight is stable — transition to OCCUPIED
          visitState = OCCUPIED;

          // Use camera ID if a fresh packet arrived
          if (camPktReady && (now - camPktTime < CAM_PKT_VALID_MS)) {
            curVisit.cat_id         = latestCamPkt.cat_id;
            curVisit.cam_confidence = latestCamPkt.confidence;
            camPktReady = false;
          }

          float cat_w = curVisit.peak_weight_g - currentBaseline;
          Serial.printf("[VISIT] Occupied — peak=%.1fg cat~%.1fg cam_id=%d(%.0f%%)\n",
            curVisit.peak_weight_g, cat_w,
            curVisit.cat_id, curVisit.cam_confidence * 100);
        }
      } else {
        // Weight dropped again (cat peeked and left) — false alarm
        visitState = IDLE;
        Serial.println("[VISIT] False alarm, back to IDLE");
      }
      break;

    // ── OCCUPIED: update peak weight ─────────────────────
    case OCCUPIED:
      curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);

      if (netWeight < CAT_EXIT_THRESHOLD_G) {
        // Weight dropped sharply — cat has left
        visitState       = LEAVING;
        curVisit.exit_ms = now;
        stableStartMs    = now;
        Serial.println("[VISIT] Cat leaving, waiting to settle...");
      }
      break;

    // ── LEAVING: wait for litter to settle ───────────────
    case LEAVING:
      if (now - stableStartMs >= (unsigned long)POST_EXIT_WAIT_MS) {
        visitState    = POST_EXIT;
        stableStartMs = now;
      }
      break;

    // ── POST_EXIT: read new baseline and log the visit ────
    case POST_EXIT:
      if (now - stableStartMs >= 2000) {
        curVisit.baseline_after_g = reading;  // stable box weight after cat left
        logVisit(curVisit);
        // Seed the rolling baseline with the new settled value
        for (int i = 0; i < BASELINE_SAMPLES; i++) baselineHistory[i] = reading;
        currentBaseline = reading;
        visitState = IDLE;
        Serial.println("[VISIT] Logged, back to IDLE");
      }
      break;
  }

  delay(20);
}
