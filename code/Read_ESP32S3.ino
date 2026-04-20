/*
   -------------------------------------------------------------------------------------
   HX711_ADC
   Arduino library for HX711 24-Bit Analog-to-Digital Converter for Weight Scales
   Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

/*
   针对 ESP32-S3 的接线：
   HX711 DT  --> ESP32-S3 D3 (GPIO 3)
   HX711 SCK --> ESP32-S3 D4 (GPIO 4)

   校准值存储在 EEPROM（ESP32 NVS 模拟），首次使用请先运行 Calibration_ESP32S3 示例获取校准值。
*/

#include <HX711_ADC.h>
#include <EEPROM.h>

// 引脚定义
const int HX711_dout = 4; // HX711 DT  --> XIAO ESP32-S3 D3 (GPIO4)
const int HX711_sck  = 5; // HX711 SCK --> XIAO ESP32-S3 D4 (GPIO5)

// HX711 构造
HX711_ADC LoadCell(HX711_dout, HX711_sck);

const int calVal_eepromAdress = 0;
unsigned long t = 0;

void setup() {
  Serial.begin(115200); delay(10);
  Serial.println();
  Serial.println("Starting...");

  LoadCell.begin();
  // LoadCell.setReverseOutput(); // 若输出值为负，取消注释可翻转输出

  float calibrationValue;
  EEPROM.begin(512);
  EEPROM.get(calVal_eepromAdress, calibrationValue);
  // 若尚未校准，可临时手动设置一个初始值，例如：
  // calibrationValue = 696.0;

  unsigned long stabilizingtime = 5000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);

  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("超时，请检查 MCU 与 HX711 的接线及引脚定义");
    while (1);
  } else {
    LoadCell.setCalFactor(calibrationValue);
    Serial.println("启动完成");
    Serial.print("使用的校准值：");
    Serial.println(calibrationValue);
  }
}

void loop() {
  static boolean newDataReady = 0;
  const int serialPrintInterval = 1000;

  if (LoadCell.update()) newDataReady = true;

  if (newDataReady) {
    if (millis() > t + serialPrintInterval) {
      float i = LoadCell.getData();
      Serial.print("Load_cell output val: ");
      Serial.println(i);
      newDataReady = 0;
      t = millis();
    }
  }

  // 串口发送 't' 触发去皮
  if (Serial.available() > 0) {
    char inByte = Serial.read();
    if (inByte == 't') LoadCell.tareNoDelay();
  }

  if (LoadCell.getTareStatus() == true) {
    Serial.println("去皮完成");
  }
}
