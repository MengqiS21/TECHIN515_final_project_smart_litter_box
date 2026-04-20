/*
   -------------------------------------------------------------------------------------
   HX711_ADC
   Arduino library for HX711 24-Bit Analog-to-Digital Converter for Weight Scales
   Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

/*
   针对 ESP32-S3 的接线测试版（无需校准）：
   HX711 DT  --> ESP32-S3 D3 (GPIO 3)
   HX711 SCK --> ESP32-S3 D4 (GPIO 4)

   用途：验证电路连接是否正常，输出为原始 ADC 值，非实际重量。
   放上重物后数值应有明显变化，说明接线正常。
*/

#include <HX711_ADC.h>

// 引脚定义
const int HX711_dout = 4; // HX711 DT  --> XIAO ESP32-S3 D3 (GPIO4)
const int HX711_sck  = 5; // HX711 SCK --> XIAO ESP32-S3 D4 (GPIO5)

// HX711 构造
HX711_ADC LoadCell(HX711_dout, HX711_sck);

unsigned long t = 0;

void setup() {
  Serial.begin(115200); delay(10);
  Serial.println();
  Serial.println("Starting (无校准测试模式)...");

  LoadCell.begin();
  LoadCell.setReverseOutput(); // 翻转输出方向（接线反向时使用）
  LoadCell.setCalFactor(1.0); // 原始 ADC 值，非实际重量

  unsigned long stabilizingtime = 2000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);

  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("超时，请检查 MCU 与 HX711 的接线及引脚定义");
    while (1);
  } else {
    Serial.println("启动完成，输出为原始 ADC 值");
  }
}

void loop() {
  static boolean newDataReady = 0;
  const int serialPrintInterval = 0;

  if (LoadCell.update()) newDataReady = true;

  if (newDataReady) {
    if (millis() > t + serialPrintInterval) {
      float i = LoadCell.getData();
      Serial.print("Raw ADC val: ");
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
