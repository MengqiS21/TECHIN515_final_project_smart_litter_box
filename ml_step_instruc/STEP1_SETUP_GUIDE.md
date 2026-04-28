# Smart Cat Litter Box Monitor
## Step 1: 摄像头测试 — 环境搭建指南

---

## 你需要的材料

| 材料 | 说明 | 大概价格 |
|------|------|----------|
| XIAO ESP32S3 Sense | 带 OV2640 摄像头扩展板 | ~$14 |
| USB-C 数据线 | 连接电脑，确保是**数据线**不是纯充电线 | 你应该有 |
| MicroSD 卡 | 8GB-32GB，**FAT32** 格式化 | ~$5 |
| 电脑 | Windows / macOS / Linux 均可 | — |

---

## Arduino IDE 配置步骤

### 1. 安装 Arduino IDE
- 下载地址：https://www.arduino.cc/en/software
- 推荐 Arduino IDE 2.x 版本

### 2. 添加 ESP32 Board Support
1. 打开 Arduino IDE
2. 进入 **File → Preferences**
3. 在 **Additional Board Manager URLs** 里添加：
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. 点 OK
5. 进入 **Tools → Board → Boards Manager**
6. 搜索 **esp32**
7. 安装 **esp32 by Espressif Systems**（选 3.x 版本）
8. 等待安装完成（需要几分钟）

### 3. 选择 Board 设置
在 **Tools** 菜单中设置：
- **Board**: XIAO_ESP32S3
- **PSRAM**: OPI PSRAM  ⚠️ **必须开启，否则摄像头不工作！**
- **Upload Speed**: 921600
- **Port**: 选择你的 USB 端口（插上板子后会出现）

### 4. 准备 SD 卡
- 用电脑把 MicroSD 卡格式化为 **FAT32**
  - Windows: 右键 → 格式化 → FAT32
  - macOS: 磁盘工具 → 抹掉 → MS-DOS (FAT)
- 插入 XIAO ESP32S3 Sense 扩展板的 SD 卡槽

---

## 烧录和测试

### 1. 打开代码
- 用 Arduino IDE 打开 `camera_test.ino`

### 2. 上传
- 用 USB-C 连接板子和电脑
- 点击 **Upload** (→ 箭头按钮)
- 等待编译和上传完成

### 3. 查看结果
- 打开 **Tools → Serial Monitor**
- 波特率设为 **115200**
- 你应该看到类似输出：
  ```
  =========================================
    Smart Cat Litter Box Monitor
    Step 1: Camera Test
  =========================================

  [Camera] PSRAM found! Using VGA (640x480)
  [Camera] Init OK!
  [SD] Card type: SDHC
  [SD] Card size: 14832 MB
  [SD] Init OK!

  [Ready] All hardware initialized!
  [Ready] Send 'p' in Serial Monitor to take a photo.
  [Ready] Or just wait — auto-captures every 5 seconds.

  --- Taking photo... ---
  [Photo] Captured! Size: 23456 bytes, Resolution: 640x480
  [SD] Saved: /photo_0001.jpg (23456 bytes)
  --- Photo saved successfully! ---
  ```

### 4. 检查照片
- 拔出 SD 卡，插到电脑上
- 你应该能看到 `photo_0001.jpg`, `photo_0002.jpg` 等文件
- 打开确认照片清晰、颜色正常

---

## 常见问题

| 问题 | 解决方法 |
|------|----------|
| Camera init FAILED | 检查 PSRAM 是否开启；确认摄像头模块插紧 |
| SD mount FAILED | 确认 SD 卡是 FAT32；确认扩展板连接正确 |
| 照片全黑 | 摄像头镜头上可能有保护膜，撕掉 |
| 照片偏色（偏绿/偏蓝） | 正常，前几帧可能颜色不准，多拍几张自动校准 |
| Serial Monitor 没输出 | 检查波特率是否 115200；按一下板子上的 Reset 按钮 |
| 找不到 USB 端口 | 换一根数据线（很多线只能充电不能传数据） |

---

## 测试成功的标志 ✓

- [ ] Serial Monitor 显示 Camera Init OK
- [ ] Serial Monitor 显示 SD Init OK
- [ ] SD 卡上有 .jpg 文件
- [ ] 照片打开后清晰可见、颜色基本正常

**全部通过后，你就可以进入 Step 2：搭建自动采集环境了！**
