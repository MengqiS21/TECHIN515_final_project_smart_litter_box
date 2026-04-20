/*
   -------------------------------------------------------------------------------------
   HX711_ADC
   Arduino library for HX711 24-Bit Analog-to-Digital Converter for Weight Scales
   Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

/*
   针对 ESP32-S3 的校准示例：
   HX711 DT  --> ESP32-S3 D3 (GPIO 3)
   HX711 SCK --> ESP32-S3 D4 (GPIO 4)

   校准步骤：
   1. 上电后，移除传感器上所有重物
   2. 串口发送 't' 进行去皮（清零）
   3. 放上已知质量的砝码
   4. 串口发送砝码质量数值（如 100.0）
   5. 根据提示选择是否将校准值保存到 EEPROM
   6. 将校准值填入 Read_ESP32S3 示例中使用

   运行中可用命令：
     't' - 去皮
     'r' - 重新校准
     'c' - 手动修改并保存校准值
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
  Serial.begin(115200);
  while (!Serial) delay(10); // 等待 USB 串口准备好（ESP32-S3 USB CDC 必须）
  Serial.println();
  Serial.println("Starting...");

  LoadCell.begin();

  unsigned long stabilizingtime = 2000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("超时，请检查 MCU 与 HX711 的接线及引脚定义");
    while (1);
  } else {
    LoadCell.setCalFactor(1.0);
    Serial.println("启动完成");
  }

  while (!LoadCell.update());
  calibrate();
}

void loop() {
  static boolean newDataReady = 0;
  const int serialPrintInterval = 0;

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

  if (Serial.available() > 0) {
    char inByte = Serial.read();
    if (inByte == 't') LoadCell.tareNoDelay();
    else if (inByte == 'r') calibrate();
    else if (inByte == 'c') changeSavedCalFactor();
  }

  if (LoadCell.getTareStatus() == true) {
    Serial.println("去皮完成");
  }
}

void calibrate() {
  Serial.println("***");
  Serial.println("开始校准：");
  Serial.println("请将传感器放在水平稳定的表面上。");
  Serial.println("移除传感器上的所有重物。");
  Serial.println("串口发送 't' 进行去皮（清零）。");

  boolean _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 't') LoadCell.tareNoDelay();
    }
    if (LoadCell.getTareStatus() == true) {
      Serial.println("去皮完成");
      _resume = true;
    }
  }

  Serial.println("请将已知质量的砝码放到传感器上。");
  Serial.println("然后从串口发送砝码质量（如 100.0）。");

  float known_mass = 0;
  _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0) {
        Serial.print("已知质量：");
        Serial.println(known_mass);
        _resume = true;
      }
    }
  }

  LoadCell.refreshDataSet();
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass);

  Serial.print("新校准值：");
  Serial.print(newCalibrationValue);
  Serial.println("，请将此值填入 Read_ESP32S3 示例中。");
  Serial.print("是否将此值保存到 EEPROM 地址 ");
  Serial.print(calVal_eepromAdress);
  Serial.println("？ y/n");

  _resume = false;
  while (_resume == false) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y') {
        EEPROM.begin(512);
        EEPROM.put(calVal_eepromAdress, newCalibrationValue);
        EEPROM.commit();
        EEPROM.get(calVal_eepromAdress, newCalibrationValue);
        Serial.print("值 ");
        Serial.print(newCalibrationValue);
        Serial.print(" 已保存到 EEPROM 地址：");
        Serial.println(calVal_eepromAdress);
        _resume = true;
      } else if (inByte == 'n') {
        Serial.println("未保存到 EEPROM");
        _resume = true;
      }
    }
  }

  Serial.println("校准结束");
  Serial.println("***");
  Serial.println("发送 'r' 重新校准，发送 'c' 手动修改校准值。");
  Serial.println("***");
}

void changeSavedCalFactor() {
  float oldCalibrationValue = LoadCell.getCalFactor();
  boolean _resume = false;
  Serial.println("***");
  Serial.print("当前校准值：");
  Serial.println(oldCalibrationValue);
  Serial.println("请从串口发送新的校准值，如 696.0");

  float newCalibrationValue;
  while (_resume == false) {
    if (Serial.available() > 0) {
      newCalibrationValue = Serial.parseFloat();
      if (newCalibrationValue != 0) {
        Serial.print("新校准值：");
        Serial.println(newCalibrationValue);
        LoadCell.setCalFactor(newCalibrationValue);
        _resume = true;
      }
    }
  }

  _resume = false;
  Serial.print("是否将此值保存到 EEPROM 地址 ");
  Serial.print(calVal_eepromAdress);
  Serial.println("？ y/n");

  while (_resume == false) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y') {
        EEPROM.begin(512);
        EEPROM.put(calVal_eepromAdress, newCalibrationValue);
        EEPROM.commit();
        EEPROM.get(calVal_eepromAdress, newCalibrationValue);
        Serial.print("值 ");
        Serial.print(newCalibrationValue);
        Serial.print(" 已保存到 EEPROM 地址：");
        Serial.println(calVal_eepromAdress);
        _resume = true;
      } else if (inByte == 'n') {
        Serial.println("未保存到 EEPROM");
        _resume = true;
      }
    }
  }
  Serial.println("修改完成");
  Serial.println("***");
}
