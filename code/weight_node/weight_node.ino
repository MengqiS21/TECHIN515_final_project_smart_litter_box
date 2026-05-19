/*
 * ============================================================
 *  Smart Cat Litter Box — Weight Node
 *  Board : XIAO ESP32S3 (无相机版，连接电脑)
 *  Role  : HX711 体重采集 + ESP-NOW 接收猫咪 ID + 访问日志
 *
 *  接线：
 *    HX711 DT  → GPIO4 (D3)
 *    HX711 SCK → GPIO5 (D4)
 *
 *  串口输出格式（供 Streamlit 读取）：
 *    实时体重：  "val:1234.5"
 *    访问记录：  "visit:{JSON}"
 *    Tare 完成： "tare:ok"
 *
 *  Serial 命令（从 Streamlit 发送）：
 *    't' → 去皮 (Tare)
 *
 *  Arduino 设置：
 *    Board: XIAO_ESP32S3 | 115200 baud
 *
 *  依赖库：
 *    - HX711_ADC (by Olav Kallhovd)
 *    - ESP32 Arduino core (内置 esp_now, WiFi)
 * ============================================================
 */

#include <HX711_ADC.h>
#include <EEPROM.h>
#include "esp_now.h"
#include "WiFi.h"

// ── HX711 引脚 ───────────────────────────────────────────────
const int HX711_dout = 4;  // DT  → GPIO4
const int HX711_sck  = 5;  // SCK → GPIO5

HX711_ADC LoadCell(HX711_dout, HX711_sck);
const int CAL_EEPROM_ADDR = 0;

// ── ESP-NOW 数据包结构（与 camera_node 保持一致） ────────────
typedef struct {
  uint8_t  cat_id;
  float    confidence;
  uint32_t timestamp_ms;
  char     method[8];
} CatIDPacket;

// ── 访问检测阈值 (克) ─────────────────────────────────────────
//   CAT_ENTER : 体重读数超过 baseline + 此值，判定猫咪进入
//   CAT_EXIT  : 体重读数低于 baseline + 此值，判定猫咪离开
//   STABLE_MS : 需要稳定多久才算"猫咪已在盒内"
const float CAT_ENTER_THRESHOLD_G = 2000.0;  // 猫咪至少 2 kg
const float CAT_EXIT_THRESHOLD_G  = 300.0;   // 离开后残留 < 300g 即为空
const int   STABLE_MS             = 2000;    // 稳定判断窗口 2 s
const int   POST_EXIT_WAIT_MS     = 3000;    // 离开后等待沉降 3 s

// ── 体重识别（体重范围映射到猫咪 ID）───────────────────────────
//   Wesley: ~4300 g,  Pupu: ~4700 g
//   允许偏差 ±300g
const float WESLEY_WEIGHT_G = 4300.0;
const float PUPU_WEIGHT_G   = 4700.0;
const float WEIGHT_TOLERANCE_G = 300.0;

// ── 状态机 ────────────────────────────────────────────────────
enum VisitState { IDLE, ENTERING, OCCUPIED, LEAVING, POST_EXIT };
VisitState visitState = IDLE;

// ── 访问数据 ──────────────────────────────────────────────────
struct VisitRecord {
  unsigned long entry_ms;
  unsigned long exit_ms;
  float  baseline_before_g;  // 猫进入前砂盆净重
  float  peak_weight_g;      // 猫在盒内最高读数
  float  baseline_after_g;   // 猫离开后稳定读数
  uint8_t cat_id;            // 0=Unknown 1=Wesley 2=Pupu
  float   cam_confidence;    // 摄像头置信度 (0=未收到)
};
VisitRecord curVisit;

// ── 基准线追踪 ────────────────────────────────────────────────
// 用 IDLE 状态下最近 N 次稳定读数的滑动均值
const int   BASELINE_SAMPLES = 20;
float       baselineHistory[BASELINE_SAMPLES];
int         baselineIdx    = 0;
bool        baselineFull   = false;
float       currentBaseline = 0.0;

void updateBaseline(float reading) {
  baselineHistory[baselineIdx] = reading;
  baselineIdx = (baselineIdx + 1) % BASELINE_SAMPLES;
  if (baselineIdx == 0) baselineFull = true;

  int n = baselineFull ? BASELINE_SAMPLES : baselineIdx;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += baselineHistory[i];
  currentBaseline = sum / n;
}

// ── ESP-NOW 接收缓存 ──────────────────────────────────────────
volatile bool     camPktReady = false;
CatIDPacket       latestCamPkt;
unsigned long     camPktTime  = 0;
const int         CAM_PKT_VALID_MS = 10000;  // 10s 内收到的摄像头数据有效

void IRAM_ATTR onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
  if (len == sizeof(CatIDPacket)) {
    memcpy((void*)&latestCamPkt, data, sizeof(CatIDPacket));
    camPktTime  = millis();
    camPktReady = true;
  }
}

// ── 串口输出每隔 500ms ─────────────────────────────────────────
unsigned long lastSerialMs = 0;

// ─────────────────────────────────────────────────────────────
//  体重→猫咪 ID（备用，摄像头无信号时使用）
// ─────────────────────────────────────────────────────────────
uint8_t identifyByCatWeight(float cat_weight_g) {
  if (fabs(cat_weight_g - WESLEY_WEIGHT_G) <= WEIGHT_TOLERANCE_G) return 1;  // Wesley
  if (fabs(cat_weight_g - PUPU_WEIGHT_G)   <= WEIGHT_TOLERANCE_G) return 2;  // Pupu
  return 0;  // Unknown
}

// ─────────────────────────────────────────────────────────────
//  访问结束：汇总并通过串口输出 JSON
// ─────────────────────────────────────────────────────────────
void logVisit(VisitRecord& v) {
  float duration_s     = (v.exit_ms - v.entry_ms) / 1000.0;
  float cat_weight_g   = v.peak_weight_g - v.baseline_after_g;
  float excrement_g    = v.baseline_after_g - v.baseline_before_g;
  if (excrement_g < 0) excrement_g = 0;  // 误差截断

  // 融合摄像头 ID + 体重 ID
  uint8_t final_cat_id   = v.cat_id;
  float   final_conf     = v.cam_confidence;
  String  id_method      = "CAM";

  if (final_cat_id == 0 || final_conf < 0.6) {
    // 摄像头没数据 or 置信度不够 → 用体重
    final_cat_id = identifyByCatWeight(cat_weight_g);
    final_conf   = (final_cat_id != 0) ? 0.85 : 0.0;
    id_method    = "WEIGHT";
  }

  const char* cat_names[] = {"Unknown", "Wesley", "Pupu"};

  // 串口输出 JSON（Streamlit health_analyzer.py 解析这行）
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
  Serial.println("[INFO] 请将上方 MAC 地址填入 camera_node 的 weightNodeMAC[]");

  // ── HX711 初始化 ─────────────────────────────────────────
  LoadCell.begin();
  float calVal;
  EEPROM.begin(512);
  EEPROM.get(CAL_EEPROM_ADDR, calVal);
  if (isnan(calVal) || calVal == 0) {
    calVal = 696.0;  // 若未校准，用默认值（先运行 Calibration_ESP32S3）
    Serial.println("[HX711] WARNING: 未找到校准值，使用默认 696.0");
  }

  LoadCell.start(5000, true);  // 5s 稳定 + 自动去皮
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("[HX711] 超时！检查接线");
    while (1) delay(100);
  }
  LoadCell.setCalFactor(calVal);
  Serial.printf("[HX711] Ready, cal=%.2f\n", calVal);

  // ── ESP-NOW 初始化 ────────────────────────────────────────
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
//  Loop — 主状态机
// ─────────────────────────────────────────────────────────────
void loop() {
  // ── HX711 读数 ───────────────────────────────────────────
  bool newData = LoadCell.update();
  float reading = 0;
  if (newData) reading = LoadCell.getData();
  else return;  // 本轮无新数据，跳过

  float netWeight = reading - currentBaseline;  // 净重（减去当前基准）
  unsigned long now = millis();

  // ── 串口实时推流（供 Streamlit 折线图）─────────────────────
  if (now - lastSerialMs >= 500) {
    Serial.printf("val:%.1f\n", reading);
    lastSerialMs = now;
  }

  // ── 串口命令处理 ─────────────────────────────────────────
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

  // ── 摄像头 ID 失效检查 ────────────────────────────────────
  if (camPktReady && (now - camPktTime > CAM_PKT_VALID_MS)) {
    camPktReady = false;
  }

  // ── 访问状态机 ───────────────────────────────────────────
  static unsigned long stableStartMs = 0;
  static float         stablePeakG   = 0;

  switch (visitState) {

    // ── 空闲：更新基准线 ──────────────────────────────────
    case IDLE:
      updateBaseline(reading);

      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        visitState = ENTERING;
        stableStartMs = now;
        stablePeakG   = netWeight;
        curVisit.entry_ms        = now;
        curVisit.baseline_before_g = currentBaseline;
        curVisit.peak_weight_g   = reading;
        curVisit.cat_id          = 0;
        curVisit.cam_confidence  = 0;
        Serial.println("[VISIT] Cat entering...");
      }
      break;

    // ── 进入：等待体重稳定，确认猫咪在盒内 ──────────────────
    case ENTERING:
      if (netWeight > stablePeakG) stablePeakG = netWeight;
      if (netWeight > CAT_ENTER_THRESHOLD_G) {
        curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);
        if (now - stableStartMs >= (unsigned long)STABLE_MS) {
          // 稳定了：正式进入 OCCUPIED
          visitState = OCCUPIED;

          // 尝试使用摄像头 ID
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
        // 体重又下去了（猫探头后离开），回到 IDLE
        visitState = IDLE;
        Serial.println("[VISIT] False alarm, back to IDLE");
      }
      break;

    // ── 在盒内：持续更新峰值体重 ─────────────────────────────
    case OCCUPIED:
      curVisit.peak_weight_g = max(curVisit.peak_weight_g, reading);

      if (netWeight < CAT_EXIT_THRESHOLD_G) {
        // 体重骤降：猫咪离开
        visitState = LEAVING;
        curVisit.exit_ms = now;
        stableStartMs    = now;
        Serial.println("[VISIT] Cat leaving, waiting to settle...");
      }
      break;

    // ── 离开中：等待砂粒沉淀后读取新基准 ────────────────────
    case LEAVING:
      if (now - stableStartMs >= (unsigned long)POST_EXIT_WAIT_MS) {
        visitState = POST_EXIT;
        stableStartMs = now;
      }
      break;

    // ── 沉淀后记录 ────────────────────────────────────────
    case POST_EXIT:
      if (now - stableStartMs >= 2000) {
        curVisit.baseline_after_g = reading;  // 猫离开后的新基准
        logVisit(curVisit);
        // 把新基准更新到滑动窗口
        for (int i = 0; i < BASELINE_SAMPLES; i++) baselineHistory[i] = reading;
        currentBaseline = reading;
        visitState = IDLE;
        Serial.println("[VISIT] Logged, back to IDLE");
      }
      break;
  }

  delay(20);
}
