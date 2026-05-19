/*
 * ============================================================
 *  Smart Cat Litter Box — Camera Node
 *  Board : XIAO ESP32S3 Sense (with camera)
 *  Role  : PIR 触发拍照 → 猫咪识别推理 → ESP-NOW 发送结果
 *
 *  接线：
 *    PIR OUT  → GPIO4 (D3)
 *    Camera   → 板载 (无需额外接线)
 *
 *  Arduino 设置：
 *    Board: XIAO_ESP32S3 | PSRAM: OPI PSRAM | 115200 baud
 *
 *  依赖库（Arduino Library Manager）：
 *    - ESP32 Arduino core (已内置 esp_camera, esp_now)
 *    - Edge Impulse SDK (cat-identifier_inferencing) — 见下方说明
 *
 *  Edge Impulse 模型集成：
 *    1. 在 Edge Impulse 导出 "Arduino library"
 *    2. 解压后添加到 Arduino/libraries/
 *    3. 取消注释下方 #include <cat-identifier_inferencing.h>
 *    4. 取消注释 runEdgeImpulseInference() 内的实际推理代码
 * ============================================================
 */

#include "esp_camera.h"
#include "esp_now.h"
#include "WiFi.h"

// ── 若已集成 Edge Impulse 模型，取消下行注释 ─────────────────
// #include <cat-identifier_inferencing.h>

// ── PIR ──────────────────────────────────────────────────────
#define PIR_PIN          4       // GPIO4 = D3
#define PIR_COOLDOWN_MS  5000   // 同一次访问内最短间隔

// ── 摄像头引脚 (XIAO ESP32S3 Sense 固定引脚) ─────────────────
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

// ── ESP-NOW 目标 MAC（填入 Weight Node 的 MAC 地址） ──────────
// 运行 weight_node 后，串口会打印本机 MAC，复制到这里
uint8_t weightNodeMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // 广播，调试阶段使用

// ── ESP-NOW 数据包结构（与 weight_node 保持一致） ────────────
typedef struct {
  uint8_t  cat_id;         // 0=Unknown 1=Wesley 2=Pupu
  float    confidence;     // 0.0 ~ 1.0
  uint32_t timestamp_ms;   // millis()
  char     method[8];      // "CAM" or "WEIGHT"
} CatIDPacket;

// ── 状态 ─────────────────────────────────────────────────────
enum CameraState { IDLE, CAT_DETECTED, COOLDOWN };
CameraState camState = IDLE;

unsigned long lastTriggerTime = 0;
int photoCount = 0;

// ── 前向声明 ─────────────────────────────────────────────────
bool initCamera();
uint8_t runInference(camera_fb_t* fb, float* out_confidence);
void sendCatID(uint8_t cat_id, float confidence);
void onDataSent(const uint8_t* mac, esp_now_send_status_t status);

// ─────────────────────────────────────────────────────────────
//  摄像头初始化
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
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240，模型输入尺寸
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
//  推理：识别是哪只猫
//  返回值：0=Unknown 1=Wesley 2=Pupu
//  out_confidence：置信度 0~1
// ─────────────────────────────────────────────────────────────
uint8_t runInference(camera_fb_t* fb, float* out_confidence) {

  /* ── 已集成 Edge Impulse 模型时，替换为以下代码 ─────────────
  if (!fb) { *out_confidence = 0; return 0; }

  // 将 JPEG 帧转为 RGB888 供模型输入
  uint8_t* rgb_buf = NULL;
  size_t   rgb_len = 0;
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, &rgb_buf, &rgb_len);
  if (!ok || !rgb_buf) { *out_confidence = 0; return 0; }

  // Edge Impulse signal
  signal_t signal;
  numpy::signal_from_buffer((float*)rgb_buf, EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3, &signal);

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  free(rgb_buf);

  if (err != EI_IMPULSE_OK) { *out_confidence = 0; return 0; }

  // 找最高置信度的标签
  float best = 0;
  int   best_idx = 0;
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best) {
      best = result.classification[i].value;
      best_idx = i;
    }
  }
  *out_confidence = best;
  // 标签顺序取决于你的 Edge Impulse 项目设置，按实际顺序映射
  if      (strcmp(result.classification[best_idx].label, "Wesley") == 0) return 1;
  else if (strcmp(result.classification[best_idx].label, "Pupu")   == 0) return 2;
  else return 0;
  ─────────────────────────────────────────────────────────── */

  // ── 模型未集成时的 Placeholder：随机返回（调试用） ──────────
  *out_confidence = 0.0;
  return 0;  // 0 = 由 weight_node 通过体重判断
}

// ─────────────────────────────────────────────────────────────
//  ESP-NOW 发送回调
// ─────────────────────────────────────────────────────────────
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  Serial.printf("[ESP-NOW] Send %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ─────────────────────────────────────────────────────────────
//  打包并发送猫咪识别结果
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
  delay(2000);  // PIR 热身

  // Camera
  if (!initCamera()) {
    Serial.println("[ERROR] Camera init failed");
  }

  // ESP-NOW — 必须在 WiFi STA 模式下初始化
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  // 注册 peer（广播或指定 MAC）
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, weightNodeMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("[Ready] Waiting for motion...");
}

// ─────────────────────────────────────────────────────────────
//  Loop — PIR 状态机
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
      // 拍照推理在 IDLE case 内同步完成，直接跳 COOLDOWN
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
