/*
 * ============================================
 *  Smart Cat Litter Box Monitor
 *  Step 1: Camera Test Sketch
 *  Board: XIAO ESP32S3 Sense
 * ============================================
 *
 *  功能：
 *  1. 初始化 OV2640 摄像头
 *  2. 拍一张照片
 *  3. 保存到 SD 卡 (photo_0001.jpg, photo_0002.jpg ...)
 *  4. 通过 Serial 打印结果
 *
 *  硬件：
 *  - XIAO ESP32S3 Sense (带 OV2640 摄像头模块)
 *  - MicroSD 卡 (FAT32 格式化)
 *  - USB-C 数据线
 *
 *  Arduino IDE 设置：
 *  - Board: "XIAO_ESP32S3"
 *  - PSRAM: "OPI PSRAM"  (必须开启!)
 *  - Upload Speed: 921600
 *  - Serial Monitor: 115200 baud
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

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

// SD 卡片选引脚 (XIAO ESP32S3 Sense 扩展板)
#define SD_CS_PIN         21

// 照片计数器
int photoCount = 0;

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
  config.frame_size   = FRAMESIZE_QVGA;  // 320x240, 够用于测试
  config.jpeg_quality = 12;               // 0-63, 数字越小质量越高
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  // 如果有 PSRAM，可以用更高分辨率
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;  // 640x480
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
//  初始化 SD 卡
// ============================================
bool initSD() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] Mount FAILED! Check:");
    Serial.println("  - SD card inserted?");
    Serial.println("  - FAT32 formatted?");
    Serial.println("  - Expansion board connected?");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] No card detected!");
    return false;
  }

  Serial.printf("[SD] Card type: %s\n",
    cardType == CARD_MMC  ? "MMC" :
    cardType == CARD_SD   ? "SD" :
    cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
  Serial.printf("[SD] Card size: %llu MB\n", SD.cardSize() / (1024 * 1024));
  Serial.println("[SD] Init OK!");
  return true;
}

// ============================================
//  拍照并保存到 SD 卡
// ============================================
bool takePhotoAndSave() {
  Serial.println("\n--- Taking photo... ---");

  // 拍照
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Photo] Capture FAILED!");
    return false;
  }

  Serial.printf("[Photo] Captured! Size: %d bytes, Resolution: %dx%d\n",
                fb->len, fb->width, fb->height);

  // 生成文件名
  photoCount++;
  char filename[32];
  sprintf(filename, "/photo_%04d.jpg", photoCount);

  // 保存到 SD 卡
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.printf("[SD] Failed to open %s for writing!\n", filename);
    esp_camera_fb_return(fb);
    return false;
  }

  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);

  Serial.printf("[SD] Saved: %s (%d bytes)\n", filename, fb->len);
  Serial.println("--- Photo saved successfully! ---");
  return true;
}

// ============================================
//  Setup
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);  // 等待 Serial 连接

  Serial.println("=========================================");
  Serial.println("  Smart Cat Litter Box Monitor");
  Serial.println("  Step 1: Camera Test");
  Serial.println("=========================================\n");

  // 初始化摄像头
  if (!initCamera()) {
    Serial.println("\n[ERROR] Camera init failed. Halting.");
    while (true) delay(1000);
  }

  // 初始化 SD 卡
  if (!initSD()) {
    Serial.println("\n[ERROR] SD card init failed. Halting.");
    while (true) delay(1000);
  }

  Serial.println("\n[Ready] All hardware initialized!");
  Serial.println("[Ready] Send 'p' in Serial Monitor to take a photo.");
  Serial.println("[Ready] Or just wait — auto-captures every 5 seconds.\n");
}

// ============================================
//  Loop
// ============================================
void loop() {
  // 方式 1：Serial 输入 'p' 手动拍照
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 'p' || cmd == 'P') {
      takePhotoAndSave();
    }
  }

  // 方式 2：每 5 秒自动拍一张（测试用，正式版删掉）
  static unsigned long lastCapture = 0;
  if (millis() - lastCapture > 5000) {
    lastCapture = millis();
    takePhotoAndSave();
  }
}
