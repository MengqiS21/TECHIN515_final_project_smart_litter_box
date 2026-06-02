/*
 * Smart Cat Litter Box — Camera Node (3-class model)
 * Board : XIAO ESP32S3 Sense  |  PSRAM: OPI PSRAM  |  115200 baud
 *
 * Model labels: "empty", "gungun" (=Wesley), "pupu" (=Pupu)
 * All three classes distinguished by camera; Wesley displays as "Wesley" everywhere.
 */

#include <catIdentifierBig.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_now.h"
#include <WiFi.h>
#include "esp_wifi.h"

// Override EI weak ei_calloc — must use C++ linkage (not extern "C"); route to PSRAM.
__attribute__((weak)) void* ei_calloc(size_t nitems, size_t size) {
    void* p = heap_caps_calloc(nitems, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(nitems, size, MALLOC_CAP_DEFAULT);
    return p;
}

// ── WiFi ─────────────────────────────────────────────────────
const char* WIFI_SSID = "MengqiPhone";
const char* WIFI_PASS = "gaoxiangshibenben";
// If camera WiFi fails: set this to weight_node serial "[WiFi] Connected! ... Ch=?"
#ifndef WIFI_FALLBACK_CHANNEL
#define WIFI_FALLBACK_CHANNEL 6
#endif

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
    Serial.println(found ? "" : " | hotspot NOT seen (2.4GHz? hotspot on?)");
    WiFi.scanDelete();
}

static bool wifiConnectStation(const char* ssid, const char* pass, int tries) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid, pass);
    Serial.printf("[WiFi] Connecting to \"%s\"", ssid);
    for (int i = 0; i < tries && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
    }
    return WiFi.status() == WL_CONNECTED;
}

// ── PIR ──────────────────────────────────────────────────────
#define PIR_PIN          2        // D1 (GPIO2)
#define PIR_COOLDOWN_MS  10000
#define PIR_WARMUP_MS    15000

// ── Camera pins (XIAO ESP32S3 Sense) ─────────────────────────
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

#define CAM_COLS 320
#define CAM_ROWS 240

// ── ESP-NOW ───────────────────────────────────────────────────
uint8_t weightNodeMAC[] = {0x1C, 0xDB, 0xD4, 0x5C, 0x8D, 0xEC};
typedef struct { uint8_t cat_id; float confidence; uint32_t timestamp_ms; char method[8]; } CatIDPacket;

// ── Globals ───────────────────────────────────────────────────
static bool          cam_ok       = false;
static int           lastPirState = LOW;
static unsigned long lastTriggerTime = 0;
static int           photoCount   = 0;
uint8_t*             snapshot_buf = nullptr;

// ── EI signal callback ────────────────────────────────────────
static int ei_camera_get_data(size_t offset, size_t length, float* out_ptr) {
    size_t pixel_ix   = offset * 3;
    size_t out_ptr_ix = 0;
    while (out_ptr_ix < length) {
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16)
                            + (snapshot_buf[pixel_ix + 1] <<  8)
                            +  snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix += 3;
    }
    return 0;
}

// ── Camera init ───────────────────────────────────────────────
bool initCamera() {
    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0; cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM; cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    if (esp_camera_init(&cfg) != ESP_OK) { Serial.println("[CAM] Init FAILED"); return false; }
    Serial.println("[CAM] Init OK");
    return true;
}

// ── Capture + resize ──────────────────────────────────────────
bool captureAndResize() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] Capture FAILED"); return false; }
    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
    esp_camera_fb_return(fb);
    if (!ok) { Serial.println("[CAM] fmt2rgb888 FAILED"); return false; }
    ei::image::processing::crop_and_interpolate_rgb888(
        snapshot_buf, CAM_COLS, CAM_ROWS,
        snapshot_buf, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);
    return true;
}

// ── Inference ─────────────────────────────────────────────────
// Labels: "empty"=0, "gungun"=Wesley=1, "pupu"=Pupu=2
uint8_t runInference(float* conf_out) {
    *conf_out = 0.0f;

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data     = &ei_camera_get_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        Serial.printf("[EI] Classifier error %d\n", err);
        return 0;
    }

    float best = 0; int bi = 0;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > best) {
            best = result.classification[i].value; bi = i;
        }
    }
    *conf_out = best;
    const char* label = result.classification[bi].label;
    // Display "Wesley" for gungun label
    const char* display = (strcasecmp(label, "gungun") == 0) ? "Wesley" : label;
    Serial.printf("[EI] label=%s conf=%.2f\n", display, best);

    if (best <= 0.7f)                          return 0;  // low confidence → weight fallback
    if (strcasecmp(label, "gungun") == 0)      return 1;  // Wesley
    if (strcasecmp(label, "pupu")   == 0)      return 2;  // Pupu
    return 0;                                              // empty → weight fallback
}

// ── ESP-NOW ───────────────────────────────────────────────────

void sendCatID(uint8_t cat_id, float conf) {
    CatIDPacket pkt;
    pkt.cat_id = cat_id; pkt.confidence = conf;
    pkt.timestamp_ms = millis();
    strncpy(pkt.method, "CAM", sizeof(pkt.method));
    esp_now_send(weightNodeMAC, (uint8_t*)&pkt, sizeof(pkt));
    const char* names[] = {"Unknown", "Wesley", "Pupu"};
    Serial.printf("[CAM] Sent cat=%s conf=%.2f\n", names[cat_id], conf);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("=== Camera Node (3-class) ===");

    if (!psramFound()) Serial.println("[WARN] PSRAM not found! Set Tools→PSRAM→OPI PSRAM");

    pinMode(PIR_PIN, INPUT);
    lastPirState = digitalRead(PIR_PIN);
    Serial.printf("[PIR] Warming up %d s...\n", PIR_WARMUP_MS / 1000);
    delay(PIR_WARMUP_MS);
    lastPirState = digitalRead(PIR_PIN);
    Serial.println("[PIR] Ready");

    // WiFi: sync ESP-NOW channel with weight_node (not for MQTT). Both must use same Ch.
    const int wifiTries = 60;  // 30 s
    bool wifiOk = wifiConnectStation(WIFI_SSID, WIFI_PASS, wifiTries);
    if (!wifiOk) {
        Serial.println("\n[WiFi] Retry once...");
        wifiOk = wifiConnectStation(WIFI_SSID, WIFI_PASS, wifiTries);
    }
    if (wifiOk) {
        Serial.printf("\n[WiFi] OK  IP=%s  Ch=%d  (weight must show same Ch)\n",
                      WiFi.localIP().toString().c_str(), WiFi.channel());
    } else {
        wl_status_t st = WiFi.status();
        Serial.printf("\n[WiFi] FAILED (status=%d", (int)st);
        if (st == WL_NO_SSID_AVAIL) Serial.print(", NO_SSID");
        else if (st == WL_CONNECT_FAILED) Serial.print(", AUTH_FAIL");
        else if (st == WL_DISCONNECTED) Serial.print(", DISCONNECTED");
        Serial.println(")");
        wifiScanForSsid(WIFI_SSID);
        esp_wifi_set_channel(WIFI_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[WiFi] ESP-NOW fallback Ch=%d — set WIFI_FALLBACK_CHANNEL to weight Ch, or fix hotspot\n",
                      WIFI_FALLBACK_CHANNEL);
    }

    cam_ok = initCamera();

    snapshot_buf = (uint8_t*)heap_caps_malloc(
        (size_t)CAM_COLS * CAM_ROWS * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot_buf) Serial.println("[CAM] snapshot_buf alloc FAILED");

    if (esp_now_init() != ESP_OK) { Serial.println("[ESP-NOW] Init FAILED"); return; }
    esp_now_register_send_cb([](const wifi_tx_info_t* info, esp_now_send_status_t st) {
        Serial.printf("[ESP-NOW] %s\n", st == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
    });
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, weightNodeMAC, 6);
    peer.channel = 0; peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("[Ready] Waiting for motion...");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    int cur = digitalRead(PIR_PIN);
    unsigned long now = millis();

    if (cur == HIGH && lastPirState == LOW) {
        if (now - lastTriggerTime > PIR_COOLDOWN_MS) {
            lastTriggerTime = now;
            photoCount++;
            Serial.printf("\n[PIR] Motion #%d\n", photoCount);
            if (cam_ok && snapshot_buf && captureAndResize()) {
                float conf = 0;
                uint8_t cat_id = runInference(&conf);
                sendCatID(cat_id, conf);
            }
        } else {
            Serial.println("[PIR] Cooldown");
        }
    }

    lastPirState = cur;
    delay(50);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor — check Edge Impulse model type"
#endif
