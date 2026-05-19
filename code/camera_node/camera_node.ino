/*
 * ============================================================
 *  Smart Cat Litter Box — Camera Node
 *  Board : XIAO ESP32S3 Sense (with camera)
 *  Role  : PIR triggers photo → cat inference → send result via ESP-NOW
 *
 *  Wiring:
 *    PIR OUT  → GPIO4 (D3)
 *    Camera   → onboard (no extra wiring needed)
 *
 *  Arduino IDE settings:
 *    Board: XIAO_ESP32S3 | PSRAM: OPI PSRAM | 115200 baud
 *
 *  Required libraries (Arduino Library Manager):
 *    - ESP32 Arduino core (esp_camera and esp_now are built in)
 *    - Edge Impulse SDK (cat-identifier_inferencing) — see note below
 *
 *  Edge Impulse model integration:
 *    1. Export "Arduino library" from your Edge Impulse project
 *    2. Unzip and add the folder to Arduino/libraries/
 *    3. Uncomment the #include <cat-identifier_inferencing.h> line below
 *    4. Uncomment the actual inference code inside runInference()
 * ============================================================
 */

#include "esp_camera.h"
#include "esp_now.h"
#include "WiFi.h"

// Uncomment when the Edge Impulse model library is installed
// #include <cat-identifier_inferencing.h>

// ── PIR ──────────────────────────────────────────────────────
#define PIR_PIN          4       // GPIO4 = D3
#define PIR_COOLDOWN_MS  5000   // minimum interval between triggers for the same visit

// ── Camera pins (fixed for XIAO ESP32S3 Sense) ───────────────
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40
#define SIOC_GPIO_NUM    39
#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15
#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

// ── Target MAC for ESP-NOW (fill in the Weight Node MAC) ─────
// Flash weight_node first; it prints its MAC on the serial monitor — copy it here
uint8_t weightNodeMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // broadcast for debugging

// ── ESP-NOW packet struct (must match weight_node) ───────────
typedef struct {
  uint8_t  cat_id;         // 0=Unknown  1=Wesley  2=Pupu
  float    confidence;     // 0.0 ~ 1.0
  uint32_t timestamp_ms;   // millis()
  char     method[8];      // "CAM" or "WEIGHT"
} CatIDPacket;

// ── State ─────────────────────────────────────────────────────
enum CameraState { IDLE, CAT_DETECTED, COOLDOWN };
CameraState camState = IDLE;

unsigned long lastTriggerTime = 0;
int photoCount = 0;

// ── Forward declarations ──────────────────────────────────────
bool initCamera();
uint8_t runInference(camera_fb_t* fb, float* out_confidence);
void sendCatID(uint8_t cat_id, float confidence);
void onDataSent(const uint8_t* mac, esp_now_send_status_t status);

// ─────────────────────────────────────────────────────────────
//  Camera initialization
// ─────────────────────────────────────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240, matches model input size
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED 0x%x\n", err);
    return false;
  }
  Serial.println("[CAM] Init OK");
  return true;
}

// ─────────────────────────────────────────────────────────────
//  Run cat inference on a captured frame
//  Returns: 0=Unknown  1=Wesley  2=Pupu
//  out_confidence: classification confidence 0~1
// ─────────────────────────────────────────────────────────────
uint8_t runInference(camera_fb_t* fb, float* out_confidence) {

  /* ── Replace this block when the Edge Impulse library is installed ──────────
  if (!fb) { *out_confidence = 0; return 0; }

  // Convert JPEG frame to RGB888 for model input
  uint8_t* rgb_buf = NULL;
  size_t   rgb_len = 0;
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, &rgb_buf, &rgb_len);
  if (!ok || !rgb_buf) { *out_confidence = 0; return 0; }

  // Build Edge Impulse signal
  signal_t signal;
  numpy::signal_from_buffer((float*)rgb_buf, EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3, &signal);

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  free(rgb_buf);

  if (err != EI_IMPULSE_OK) { *out_confidence = 0; return 0; }

  // Find the label with the highest confidence
  float best = 0;
  int   best_idx = 0;
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best) {
      best = result.classification[i].value;
      best_idx = i;
    }
  }
  *out_confidence = best;
  // Label order depends on your Edge Impulse project — map accordingly
  if      (strcmp(result.classification[best_idx].label, "Wesley") == 0) return 1;
  else if (strcmp(result.classification[best_idx].label, "Pupu")   == 0) return 2;
  else return 0;
  ────────────────────────────────────────────────────────────────────────── */

  // Placeholder while model is not yet integrated — weight_node uses weight-based ID instead
  *out_confidence = 0.0;
  return 0;
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW send callback
// ─────────────────────────────────────────────────────────────
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  Serial.printf("[ESP-NOW] Send %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ─────────────────────────────────────────────────────────────
//  Pack and send cat identification result via ESP-NOW
// ─────────────────────────────────────────────────────────────
void sendCatID(uint8_t cat_id, float confidence) {
  CatIDPacket pkt;
  pkt.cat_id       = cat_id;
  pkt.confidence   = confidence;
  pkt.timestamp_ms = millis();
  strncpy(pkt.method, "CAM", sizeof(pkt.method));

  esp_now_send(weightNodeMAC, (uint8_t*)&pkt, sizeof(pkt));

  const char* names[] = {"Unknown", "Wesley", "Pupu"};
  Serial.printf("[CAM] Sent → cat=%s conf=%.2f\n", names[cat_id], confidence);
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=== Camera Node (ESP-NOW Sender) ===");
  Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());

  // PIR
  pinMode(PIR_PIN, INPUT);
  Serial.println("[PIR] Ready on GPIO4");
  delay(2000);  // PIR warm-up

  // Camera
  if (!initCamera()) {
    Serial.println("[ERROR] Camera init failed");
  }

  // ESP-NOW must be initialized in WiFi STA mode
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  // Register peer (broadcast or specific MAC)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, weightNodeMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[Ready] Waiting for motion...");
}

// ─────────────────────────────────────────────────────────────
//  Loop — PIR state machine
// ─────────────────────────────────────────────────────────────
void loop() {
  int pirVal = digitalRead(PIR_PIN);
  unsigned long now = millis();

  switch (camState) {

    case IDLE:
      if (pirVal == HIGH) {
        camState = CAT_DETECTED;
        lastTriggerTime = now;
        photoCount++;
        Serial.printf("\n[PIR] Motion! Photo #%d\n", photoCount);

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
          Serial.println("[CAM] Capture FAILED");
          camState = COOLDOWN;
          break;
        }

        float conf = 0;
        uint8_t cat_id = runInference(fb, &conf);
        esp_camera_fb_return(fb);

        Serial.printf("[CAM] Inference: cat=%d conf=%.2f\n", cat_id, conf);
        sendCatID(cat_id, conf);

        camState = COOLDOWN;
      }
      break;

    case CAT_DETECTED:
      // Capture and inference are handled synchronously in the IDLE case — jump to COOLDOWN
      camState = COOLDOWN;
      break;

    case COOLDOWN:
      if (now - lastTriggerTime > PIR_COOLDOWN_MS) {
        camState = IDLE;
        Serial.println("[PIR] Cooldown over, ready.");
      }
      break;
  }

  delay(50);
}
