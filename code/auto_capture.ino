/*
 * ============================================
 *  Smart Cat Litter Box Monitor
 *  Step 2: PIR Auto-Capture Sketch
 *  Board: XIAO ESP32S3 Sense
 * ============================================
 *
 *  功能：
 *  1. PIR 传感器检测到运动 → 自动拍照
 *  2. 每次触发连拍 3 张（提高有效照片率）
 *  3. 照片存到 SD 卡，文件名自动递增
 *  4. 冷却期防止重复触发（同一只猫一次上厕所只拍一组）
 *  5. Serial 打印拍照日志
 *
 *  硬件接线：
 *  - PIR 传感器:
 *      VCC  → 3.3V
 *      GND  → GND
 *      OUT  → D2 (GPIO 3)
 *  - 摄像头和 SD 卡用扩展板自带的，不用接线
 *
 *  采集策略：
 *  - 让设备在猫砂盆旁跑 3-5 天
 *  - 之后取出 SD 卡，人工分类照片到 cat_a/ 和 cat_b/ 文件夹
 *  - 每只猫目标 150-200 张有效照片
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// ============================================
//  引脚定义
// ============================================

// XIAO ESP32S3 Sense 摄像头引脚
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// SD 卡
#define SD_CS_PIN         21

// PIR 传感器
#define PIR_PIN           3    // D2 on XIAO ESP32S3

// ============================================
//  配置参数（可以根据实际情况调整）
// ============================================
#define BURST_COUNT       3        // 每次触发连拍几张
#define BURST_INTERVAL    800      // 连拍间隔 (ms)，给猫时间换姿势
#define COOLDOWN_TIME     30000    // 冷却期 (ms)，防止同一次上厕所重复拍
#define PIR_WARMUP_TIME   10000    // PIR 传感器预热时间 (ms)

// ============================================
//  全局变量
// ============================================
int photoCount = 0;
unsigned long lastTriggerTime = 0;
int triggerCount = 0;  // 总触发次数

// ============================================
//  初始化摄像头
// ============================================
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
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;   // 640x480
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    Serial.println("[Camera] PSRAM found, using VGA (640x480)");
  } else {
    Serial.println("[Camera] No PSRAM, using QVGA (320x240)");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init FAILED: 0x%x\n", err);
    return false;
  }
  Serial.println("[Camera] Init OK!");
  return true;
}

// ============================================
//  初始化 SD 卡
// ============================================
bool initSD() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED!");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] No card detected!");
    return false;
  }

  // 读取已有照片数量，从最大编号继续（防止重启后覆盖）
  File root = SD.open("/");
  while (File entry = root.openNextFile()) {
    String name = entry.name();
    if (name.startsWith("img_") && name.endsWith(".jpg")) {
      // 从文件名 "img_00042.jpg" 提取数字 42
      int num = name.substring(4, 9).toInt();
      if (num > photoCount) {
        photoCount = num;
      }
    }
    entry.close();
  }
  root.close();

  Serial.printf("[SD] Init OK! Card size: %llu MB\n", SD.cardSize() / (1024 * 1024));
  Serial.printf("[SD] Found %d existing photos, will continue from img_%05d.jpg\n",
                photoCount, photoCount + 1);
  return true;
}

// ============================================
//  拍一张照片并保存
// ============================================
bool captureAndSave() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Photo] Capture failed!");
    return false;
  }

  photoCount++;
  char filename[32];
  sprintf(filename, "/img_%05d.jpg", photoCount);

  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.printf("[SD] Cannot open %s\n", filename);
    esp_camera_fb_return(fb);
    return false;
  }

  file.write(fb->buf, fb->len);
  file.close();

  Serial.printf("  [Photo] Saved: %s (%d bytes, %dx%d)\n",
                filename, fb->len, fb->width, fb->height);

  esp_camera_fb_return(fb);
  return true;
}

// ============================================
//  连拍一组照片
// ============================================
void burstCapture() {
  triggerCount++;
  Serial.printf("\n=== Trigger #%d | Taking %d photos ===\n",
                triggerCount, BURST_COUNT);

  int success = 0;
  for (int i = 0; i < BURST_COUNT; i++) {
    if (captureAndSave()) {
      success++;
    }
    if (i < BURST_COUNT - 1) {
      delay(BURST_INTERVAL);  // 连拍间隔
    }
  }

  Serial.printf("=== Done: %d/%d saved | Total photos: %d ===\n\n",
                success, BURST_COUNT, photoCount);
}

// ============================================
//  Setup
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=========================================");
  Serial.println("  Smart Cat Litter Box Monitor");
  Serial.println("  Step 2: PIR Auto-Capture");
  Serial.println("=========================================\n");

  // 初始化摄像头
  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed!");
    while (true) delay(1000);
  }

  // 初始化 SD 卡
  if (!initSD()) {
    Serial.println("[FATAL] SD card init failed!");
    while (true) delay(1000);
  }

  // 初始化 PIR
  pinMode(PIR_PIN, INPUT);
  Serial.println("[PIR] Pin configured");

  // PIR 需要预热
  Serial.printf("[PIR] Warming up (%d seconds)...\n", PIR_WARMUP_TIME / 1000);
  delay(PIR_WARMUP_TIME);

  Serial.println("\n[Ready] System armed! Waiting for cat...\n");
  Serial.println("  Burst count:  " + String(BURST_COUNT) + " photos per trigger");
  Serial.println("  Cooldown:     " + String(COOLDOWN_TIME / 1000) + " seconds");
  Serial.println("  Send 't' in Serial to test capture manually\n");
}

// ============================================
//  Loop
// ============================================
void loop() {
  // 手动触发（调试用）
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 't' || cmd == 'T') {
      Serial.println("[Manual] Test capture triggered");
      burstCapture();
      lastTriggerTime = millis();
    }
  }

  // PIR 检测
  if (digitalRead(PIR_PIN) == HIGH) {
    unsigned long now = millis();

    // 检查冷却期
    if (now - lastTriggerTime > COOLDOWN_TIME) {
      Serial.println("[PIR] Motion detected!");
      burstCapture();
      lastTriggerTime = now;
    } else {
      // 冷却期内，忽略
      // (不打印，避免刷屏)
    }
  }

  delay(100);  // 检测间隔 100ms
}
