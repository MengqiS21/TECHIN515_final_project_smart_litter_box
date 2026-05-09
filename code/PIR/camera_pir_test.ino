/*
 * ============================================
 *  Smart Cat Litter Box Monitor
 *  PIR + Camera Test (No SD Card)
 *  Board: XIAO ESP32S3 Sense
 * ============================================
 *
 *  功能：
 *  1. PIR 传感器检测到动作
 *  2. 摄像头拍一张照片
 *  3. 通过 Serial 打印拍照结果（大小、分辨率）
 *  4. 无需 SD 卡
 *
 *  硬件接线：
 *  - PIR GND  → XIAO GND
 *  - PIR 3V3  → XIAO 3V3
 *  - PIR OUT  → XIAO D3 (GPIO3)
 *
 *  Arduino IDE 设置：
 *  - Board: "XIAO_ESP32S3"
 *  - PSRAM: "OPI PSRAM"  (必须开启!)
 *  - Serial Monitor: 115200 baud
 */

#include "esp_camera.h"

// ============================================
//  PIR 引脚
// ============================================
#define PIR_PIN   4   // D3 = GPIO4

// ============================================
//  XIAO ESP32S3 Sense 摄像头引脚定义
// ============================================
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

// 拍照计数
int photoCount = 0;

// PIR 防抖：触发后冷却时间（毫秒）
#define PIR_COOLDOWN_MS  3000
unsigned long lastTriggerTime = 0;

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
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    Serial.println("[Camera] PSRAM found! Using VGA (640x480)");
  } else {
    Serial.println("[Camera] No PSRAM, using QVGA (320x240)");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Init FAILED! Error: 0x%x\n", err);
    return false;
  }

  Serial.println("[Camera] Init OK!");
  return true;
}

// ============================================
//  拍照（无 SD 卡，结果打印到 Serial）
// ============================================
void takePhoto() {
  photoCount++;
  Serial.printf("\n[PIR] Motion detected! Taking photo #%d...\n", photoCount);

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Camera] Capture FAILED!");
    return;
  }

  Serial.println("[Camera] Capture SUCCESS!");
  Serial.printf("  Resolution : %d x %d\n", fb->width, fb->height);
  Serial.printf("  Size       : %d bytes (%.1f KB)\n", fb->len, fb->len / 1024.0);
  Serial.printf("  Format     : JPEG\n");
  Serial.printf("  Photo #    : %d\n", photoCount);
  Serial.println("[Camera] (No SD card — photo not saved, camera is working normally)");

  esp_camera_fb_return(fb);
}

// ============================================
//  Setup
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=========================================");
  Serial.println("  PIR + Camera Test (No SD Card)");
  Serial.println("=========================================\n");

  // PIR 初始化
  pinMode(PIR_PIN, INPUT);
  Serial.println("[PIR] Initialized on D3 (GPIO3)");

  // 等待 PIR 稳定（HC-SR501 需要约 30 秒热身，这里等 2 秒做基本稳定）
  Serial.println("[PIR] Warming up (2s)...");
  delay(2000);
  Serial.println("[PIR] Ready!");

  // 摄像头初始化（失败只警告，不死循环）
  if (!initCamera()) {
    Serial.println("[ERROR] Camera init failed! Check camera module connection.");
    Serial.println("[INFO]  PIR will still run, but photos won't capture.");
  }

  Serial.println("\n[Ready] Waiting for motion...\n");
}

// ============================================
//  Loop
// ============================================
void loop() {
  int pirState = digitalRead(PIR_PIN);

  if (pirState == HIGH) {
    unsigned long now = millis();
    // 冷却时间内不重复触发
    if (now - lastTriggerTime > PIR_COOLDOWN_MS) {
      lastTriggerTime = now;
      takePhoto();
      Serial.println("\n[PIR] Cooldown 3s — ignoring further triggers...");
    }
  }

  delay(50);  // 轻微降频，避免 Serial 刷太快
}
