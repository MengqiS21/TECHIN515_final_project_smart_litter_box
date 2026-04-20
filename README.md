# Smart Cat Litter Box — TECHIN 515 Final Project

An IoT-enabled smart litter box that uses a load cell weight sensor to monitor cat usage patterns and track cat weight over time. The system is built around a Seeed Studio XIAO ESP32-S3 microcontroller and an HX711 24-bit ADC for high-precision weight measurement.

---

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| Microcontroller | Seeed Studio XIAO ESP32-S3 |
| ADC Module | HX711 24-Bit Load Cell Amplifier |
| Sensor | Load cell (rated for expected weight range) |
| Cable | USB-C (for programming and serial monitor) |
| Enclosure | 3D-printed base (see `Base.stl`) |

---

## Wiring

| HX711 Pin | XIAO ESP32-S3 Pin | GPIO |
|-----------|-------------------|------|
| DT (Data) | D3 | GPIO 4 |
| SCK (Clock) | D4 | GPIO 5 |
| VCC | 3.3V | — |
| GND | GND | — |

Connect the load cell's four wires to the HX711 module following the color convention printed on the HX711 board (typically: Red=E+, Black=E−, White=A−, Green=A+).

---

## Software Requirements

### 1. Arduino IDE
Download and install **Arduino IDE 2.x** from https://www.arduino.cc/en/software

### 2. ESP32-S3 Board Support
1. Open Arduino IDE → **File → Preferences**
2. In *Additional boards manager URLs*, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**, search for `esp32`, and install **esp32 by Espressif Systems** (version 2.x or later)

### 3. Required Library
Install via **Tools → Manage Libraries**:

| Library | Author | Version |
|---------|--------|---------|
| `HX711_ADC` | Olav Kallhovd | Latest |

Search for `HX711_ADC` and click **Install**.

> See `code/libraries.txt` for a machine-readable list of required libraries.

---

## Board Configuration in Arduino IDE

Go to **Tools** and set:

| Setting | Value |
|---------|-------|
| Board | XIAO_ESP32S3 |
| Port | (select the COM/tty port that appears when you plug in the board) |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |

---

## Code Structure

```
code/
├── Read_ESP32S3_NoCalib(1).ino   # Step 1 — Connection test (raw ADC, no calibration)
├── Calibration_ESP32S3.ino       # Step 2 — Sensor calibration, saves value to EEPROM
└── Read_ESP32S3.ino              # Step 3 — Production use with calibration applied
```

> **Important (Arduino IDE requirement):** When you open an `.ino` file that is not inside a folder with the same name, Arduino IDE will prompt you to create a matching folder and move the file. Click **OK** to accept — this is required behavior and does not affect the code.

---

## Running the Code — Step by Step

### Step 1: Verify Circuit Connection (No Calibration)

Use this sketch first to confirm that the load cell and HX711 are wired correctly.

1. Open `Read_ESP32S3_NoCalib(1).ino` in Arduino IDE
2. Select the correct board and port (see Board Configuration above)
3. Click **Upload**
4. Open **Tools → Serial Monitor**, set baud rate to **115200**
5. You should see raw ADC values printing continuously
6. Place a weight on the load cell — the values should change noticeably
7. If values change, wiring is correct. Proceed to Step 2.

**Serial commands:**
- `t` — Tare (zero) the sensor

---

### Step 2: Calibrate the Sensor

Run this sketch once to calculate and save a calibration factor to EEPROM.

1. Open `Calibration_ESP32S3.ino` in Arduino IDE
2. Upload to the XIAO ESP32-S3
3. Open **Serial Monitor** at **115200 baud**
4. Follow the on-screen prompts:
   - Remove all weight from the load cell
   - Send `t` to tare (zero) the sensor
   - Place a **known-weight object** (e.g., 100.0 g) on the load cell
   - Type the exact weight value (e.g., `100.0`) in the Serial Monitor and press Enter
   - When asked, send `y` to save the calibration value to EEPROM
5. Note the printed calibration value — it is also saved automatically to EEPROM address 0

**Serial commands during calibration:**
- `t` — Tare
- `r` — Restart calibration
- `c` — Manually enter and save a new calibration factor

---

### Step 3: Read Actual Weight

After calibration, use this sketch for normal operation.

1. Open `Read_ESP32S3.ino` in Arduino IDE
2. Upload to the XIAO ESP32-S3
3. Open **Serial Monitor** at **115200 baud**
4. The sketch automatically reads the calibration factor from EEPROM and prints the calibration value used
5. Weight readings (in grams, relative to calibration) are printed every **1 second**

**Serial commands:**
- `t` — Tare (re-zero the sensor)

> If weight readings appear as large negative numbers, uncomment `LoadCell.setReverseOutput();` in the sketch (line 36) and re-upload.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Timeout error on startup | Incorrect wiring or wrong GPIO pins | Double-check DT→GPIO4, SCK→GPIO5 |
| No serial output | USB CDC not enabled or wrong port | Enable USB CDC On Boot in board settings |
| Values don't change when weight added | Loose connection or broken load cell | Re-seat all connections |
| Calibration value reads as `nan` or `0` | EEPROM not yet written | Run calibration sketch first |
| Weight reads negative | Load cell wired in reverse | Uncomment `setReverseOutput()` |

---

## Repository Contents

| File | Description |
|------|-------------|
| `code/Calibration_ESP32S3.ino` | Calibration sketch for ESP32-S3 |
| `code/Read_ESP32S3.ino` | Production weight reading sketch |
| `code/Read_ESP32S3_NoCalib(1).ino` | Connection test sketch (raw ADC) |
| `code/libraries.txt` | Required Arduino libraries |
| `Base.stl` | 3D model of the litter box base |
| `TECHIN515_Budget.pdf` | Project budget breakdown |
| `Prototype1_HX711_Weight_Sensor_Test_Report.pdf` | Milestone 1 test report |
| `Smart_Cat_Litter_Box_Milestone1(3).pptx` | Project presentation slides |

---

## Authors

Xin Luo, Yuna Xiong, Mengqi Shi — Spring 2026
