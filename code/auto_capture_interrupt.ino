/*
 * ============================================
 *  Smart Cat Litter Box Monitor
 *  Step 2: PIR Auto-Capture (Interrupt Version)
 *  Board: XIAO ESP32S3 Sense
 * ============================================
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// XIAO ESP32S3 Sense camera pins
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

#define SD_CS_PIN         21
#define PIR_PIN           1    // D0 (GPIO 1) - change if you use a different pin

#define BURST_COUNT       3
#define BURST_INTERVAL    800
#define COOLDOWN_TIME     30000
#define PIR_WARMUP_TIME   15000   // 15 seconds warmup for stability

// Global variables
int photoCount = 0;
unsigned long lastTriggerTime = 0;
int triggerCount = 0;

// Interrupt flag - volatile because modified in ISR
volatile bool motionDetected = false;

// ISR - keep it minimal
void IRAM_ATTR onMotion() {
  motionDetected = true;
}

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

  File root = SD.open("/");
  while (File entry = root.openNextFile()) {
    String name = entry.name();
    if (name.startsWith("img_") && name.endsWith(".jpg")) {
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
      delay(BURST_INTERVAL);
    }
  }

  Serial.printf("=== Done: %d/%d saved | Total photos: %d ===\n\n",
                success, BURST_COUNT, photoCount);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=========================================");
  Serial.println("  Smart Cat Litter Box Monitor");
  Serial.println("  Step 2: PIR Auto-Capture (Interrupt)");
  Serial.println("=========================================\n");

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed!");
    while (true) delay(1000);
  }

  if (!initSD()) {
    Serial.println("[FATAL] SD card init failed!");
    while (true) delay(1000);
  }

  // Setup PIR with interrupt
  pinMode(PIR_PIN, INPUT);
  
  // PIR warmup
  Serial.printf("[PIR] Warming up (%d seconds)...\n", PIR_WARMUP_TIME / 1000);
  delay(PIR_WARMUP_TIME);

  // Attach interrupt AFTER warmup to avoid false triggers
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onMotion, RISING);
  Serial.println("[PIR] Interrupt attached on GPIO " + String(PIR_PIN));

  Serial.println("\n[Ready] System armed! Waiting for cat...\n");
  Serial.println("  Burst count:  " + String(BURST_COUNT) + " photos per trigger");
  Serial.println("  Cooldown:     " + String(COOLDOWN_TIME / 1000) + " seconds");
  Serial.println("  Send 't' in Serial to test capture manually\n");
}

void loop() {
  // Manual trigger
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 't' || cmd == 'T') {
      Serial.println("[Manual] Test capture triggered");
      burstCapture();
      lastTriggerTime = millis();
    }
  }

  // Check interrupt flag
  if (motionDetected) {
    motionDetected = false;  // Reset flag
    unsigned long now = millis();

    if (now - lastTriggerTime > COOLDOWN_TIME) {
      Serial.println("[PIR] Motion detected via interrupt!");
      burstCapture();
      lastTriggerTime = now;
    } else {
      Serial.println("[PIR] Cooldown active, ignoring trigger");
    }
  }

  delay(100);
}
