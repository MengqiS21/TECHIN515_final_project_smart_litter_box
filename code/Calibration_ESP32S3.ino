/*
   -------------------------------------------------------------------------------------
   HX711_ADC
   Arduino library for HX711 24-Bit Analog-to-Digital Converter for Weight Scales
   Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

/*
   Calibration example for ESP32-S3:
   HX711 DT  --> ESP32-S3 D3 (GPIO 3)
   HX711 SCK --> ESP32-S3 D4 (GPIO 4)

   Calibration steps:
   1. After power on, remove all weight from the sensor
   2. Send 't' via serial to tare (zero out)
   3. Place a known mass (calibration weight) on the sensor
   4. Send the weight value via serial (e.g. 100.0)
   5. Follow the prompt to save the calibration value to EEPROM
   6. Use the calibration value in the Read_ESP32S3 example

   Available commands during operation:
     't' - Tare
     'r' - Recalibrate
     'c' - Manually change and save calibration value
*/

#include <HX711_ADC.h>
#include <EEPROM.h>

// Pin definitions
const int HX711_dout = 4; // HX711 DT  --> XIAO ESP32-S3 D3 (GPIO4)
const int HX711_sck  = 5; // HX711 SCK --> XIAO ESP32-S3 D4 (GPIO5)

// HX711 constructor
HX711_ADC LoadCell(HX711_dout, HX711_sck);

const int calVal_eepromAdress = 0;
unsigned long t = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10); // Wait for USB serial to be ready (required for ESP32-S3 USB CDC)
  Serial.println();
  Serial.println("Starting...");

  LoadCell.begin();

  unsigned long stabilizingtime = 2000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);

  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("Timeout, check MCU and HX711 connection and pins");
    while (1);
  } else {
    LoadCell.setCalFactor(1.0);
    Serial.println("Startup OK");
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
    Serial.println("Tare complete");
  }
}

void calibrate() {
  Serial.println("***");
  Serial.println("Starting calibration:");
  Serial.println("Place the sensor on a flat, stable surface.");
  Serial.println("Remove all weight from the sensor.");
  Serial.println("Send 't' via serial to tare (zero out).");

  boolean _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 't') LoadCell.tareNoDelay();
    }
    if (LoadCell.getTareStatus() == true) {
      Serial.println("Tare complete");
      _resume = true;
    }
  }

  Serial.println("Place a known mass on the sensor.");
  Serial.println("Then send the mass value via serial (e.g. 100.0).");

  float known_mass = 0;
  _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0) {
        Serial.print("Known mass: ");
        Serial.println(known_mass);
        _resume = true;
      }
    }
  }

  LoadCell.refreshDataSet();
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass);

  Serial.print("New calibration value: ");
  Serial.print(newCalibrationValue);
  Serial.println(", use this value in the Read_ESP32S3 example.");
  Serial.print("Save this value to EEPROM address ");
  Serial.print(calVal_eepromAdress);
  Serial.println("? y/n");

  _resume = false;
  while (_resume == false) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y') {
        EEPROM.begin(512);
        EEPROM.put(calVal_eepromAdress, newCalibrationValue);
        EEPROM.commit();
        EEPROM.get(calVal_eepromAdress, newCalibrationValue);
        Serial.print("Value ");
        Serial.print(newCalibrationValue);
        Serial.print(" saved to EEPROM address: ");
        Serial.println(calVal_eepromAdress);
        _resume = true;
      } else if (inByte == 'n') {
        Serial.println("Value not saved to EEPROM");
        _resume = true;
      }
    }
  }

  Serial.println("Calibration complete");
  Serial.println("***");
  Serial.println("Send 'r' to recalibrate, send 'c' to manually change calibration value.");
  Serial.println("***");
}

void changeSavedCalFactor() {
  float oldCalibrationValue = LoadCell.getCalFactor();
  boolean _resume = false;
  Serial.println("***");
  Serial.print("Current calibration value: ");
  Serial.println(oldCalibrationValue);
  Serial.println("Send new calibration value via serial, e.g. 696.0");

  float newCalibrationValue;
  while (_resume == false) {
    if (Serial.available() > 0) {
      newCalibrationValue = Serial.parseFloat();
      if (newCalibrationValue != 0) {
        Serial.print("New calibration value: ");
        Serial.println(newCalibrationValue);
        LoadCell.setCalFactor(newCalibrationValue);
        _resume = true;
      }
    }
  }

  _resume = false;
  Serial.print("Save this value to EEPROM address ");
  Serial.print(calVal_eepromAdress);
  Serial.println("? y/n");

  while (_resume == false) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y') {
        EEPROM.begin(512);
        EEPROM.put(calVal_eepromAdress, newCalibrationValue);
        EEPROM.commit();
        EEPROM.get(calVal_eepromAdress, newCalibrationValue);
        Serial.print("Value ");
        Serial.print(newCalibrationValue);
        Serial.print(" saved to EEPROM address: ");
        Serial.println(calVal_eepromAdress);
        _resume = true;
      } else if (inByte == 'n') {
        Serial.println("Value not saved to EEPROM");
        _resume = true;
      }
    }
  }
  Serial.println("Change complete");
  Serial.println("***");
}