# Step 4: Edge Impulse 模型训练完整指南
## 用 Edge Impulse 训练猫咪识别模型并部署到 ESP32S3 Sense

---

## 前提条件

在开始之前，你需要：
- 完成 Step 2-3，SD 卡里有标注好的两个文件夹：`cat_a/`（150+ 张）和 `cat_b/`（150+ 张）
- 一台能上网的电脑
- 一个 Edge Impulse 免费账号

---

## Part 1：注册 Edge Impulse 并创建项目

1. 打开 https://studio.edgeimpulse.com/
2. 点击 **Sign up**，用邮箱注册一个免费账号（用学校邮箱 .edu 也可以）
3. 登录后，点击右上角 **Create new project**
4. 项目名称填：`Cat-Identifier-LitterBox`（或任意你喜欢的名字）
5. 选择 **Images** 作为项目类型
6. 点击 **Create**

---

## Part 2：上传照片数据

1. 进入项目后，点击左侧菜单栏的 **Data acquisition**
2. 点击上方的 **Add data** → **Upload data**
3. 上传方式选择 **Select a folder**

### 上传 Cat A 的照片：
- 选择你电脑上的 `cat_a/` 文件夹
- **Label** 填：`cat_a`
- **Upload into category** 选：**Automatically split between training and testing**（Edge Impulse 会自动按 80/20 分成训练集和测试集）
- 点击 **Upload**，等待所有照片上传完成

### 上传 Cat B 的照片：
- 重复上面的步骤，选择 `cat_b/` 文件夹
- **Label** 填：`cat_b`
- 同样选 **Automatically split**
- 点击 **Upload**

### 检查数据：
- 上传完成后，在 **Data acquisition** 页面可以看到所有样本
- 点击顶部的 **Training data** 和 **Test data** 标签切换查看
- 确认两个类别的数量大致平衡（差距不超过 2:1）
- 随机点开几张确认标签正确、照片清晰

---

## Part 3：设计 Impulse（数据处理 Pipeline）

这一步是定义数据从输入到输出的流程。

1. 点击左侧菜单的 **Create impulse**（也叫 Impulse design）
2. 你会看到一个 pipeline 配置界面，按以下方式设置：

### Image data（输入块）：
- **Image width**: `96`
- **Image height**: `96`
- **Resize mode**: `Fit shortest axis`（保持比例缩放后裁剪为正方形）

### 添加 Processing block：
- 点击 **Add a processing block**
- 选择 **Image**（这个块会把原始图片转成模型需要的格式）

### 添加 Learning block：
- 点击 **Add a learning block**
- 选择 **Transfer Learning (Images)**（这就是 MobileNetV2 方案）
- 注意：不要选 "Classification" 里的普通 NN，Transfer Learning 效果好得多

3. 点击 **Save Impulse**

---

## Part 4：配置图像处理参数

1. 点击左侧菜单的 **Image**（在 Impulse design 下面）
2. **Color depth** 选择：**RGB**（猫的毛色是重要特征，保留颜色信息）
3. 点击 **Save parameters**
4. 点击 **Generate features**

这一步会处理你所有的训练图片，生成特征向量。处理完后你会看到一个 2D 可视化图（Feature explorer），如果两只猫的数据点在图上能明显分开，说明数据质量很好，模型训练效果会不错。

---

## Part 5：训练模型

1. 点击左侧菜单的 **Transfer Learning**（在 Image 下面）
2. 配置训练参数：

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| Number of training cycles | `50` | 先用 50 个 epoch，如果效果不好再加到 100 |
| Learning rate | `0.0005` | 默认值，通常不需要改 |
| Minimum confidence rating | `0.6` | 低于 60% 置信度的预测视为"不确定" |
| Data augmentation | **开启** | 自动翻转、旋转、亮度调节，增加数据多样性 |

3. 在 **Neural network architecture** 部分：
   - 选择 **MobileNetV2 96x96 0.1**（最小的 MobileNetV2 变体）
   - 这个模型针对 96×96 输入优化，专为资源受限的 MCU 设计
   - 如果看不到这个选项，选择 **MobileNetV2 96x96 0.05** 也行

4. 点击 **Start training**
5. 等待训练完成（通常 5-15 分钟）

### 查看训练结果：
- **Accuracy**（准确率）：目标 > 90%，如果 > 85% 也可以接受
- **Loss**（损失）：越小越好
- **Confusion matrix**（混淆矩阵）：看 cat_a 和 cat_b 分别有多少被正确分类
  - 对角线数字越高越好（正确分类）
  - 非对角线数字越低越好（错误分类）

### 如果准确率低于 85%，尝试：
- 增加训练数据（每只猫加到 200-300 张）
- 开启数据增强（Data augmentation）
- 增加 training cycles 到 100
- 检查数据质量：删掉模糊的、猫不在画面中的、光线太暗的照片

---

## Part 6：测试模型

1. 点击左侧菜单的 **Model testing**
2. 点击 **Classify all**
3. Edge Impulse 会用之前自动分出来的测试集验证模型
4. 查看测试准确率，这个数字比训练准确率更可靠（因为模型没见过测试集的数据）
5. 测试准确率 > 85% 就可以进入部署

---

## Part 7：导出 Arduino Library

1. 点击左侧菜单的 **Deployment**
2. 在搜索框搜索 **Arduino library**
3. 选择 **Arduino library**
4. 下方的优化选项：
   - **Quantized (int8)**: 选择这个！模型更小、推理更快，适合 ESP32
   - 不要选 Float32，那个太大了
5. 点击 **Build**
6. 等待构建完成，浏览器会自动下载一个 `.zip` 文件
   - 文件名类似：`ei-cat-identifier-litterbox-arduino-1.0.1.zip`
   - 不要解压这个文件！

---

## Part 8：在 Arduino IDE 中安装库并运行

### 安装库：
1. 打开 Arduino IDE
2. 点击 **Sketch → Include Library → Add .ZIP Library...**
3. 选择刚才下载的 `.zip` 文件
4. 等待安装完成，底部状态栏会显示 "Library added to your libraries"

### 运行示例测试：
1. 点击 **File → Examples → 你的项目名 - Edge Impulse → esp32 → esp32_camera**
   - 如果找不到，试试重启 Arduino IDE
2. 这个示例代码会用 ESP32S3 的摄像头实时拍照并推理
3. 确认代码中的摄像头引脚定义和你的 XIAO ESP32S3 Sense 一致：
   ```
   #define CAMERA_MODEL_XIAO_ESP32S3
   ```
4. Board 设置：
   - Board: **XIAO_ESP32S3**
   - PSRAM: **OPI PSRAM**（必须开启！）
5. 上传到板子
6. 打开 Serial Monitor（115200 baud）
7. 你应该能看到类似输出：
   ```
   Predictions:
     cat_a: 0.92
     cat_b: 0.05
   ```

### 如果示例代码中没有 XIAO ESP32S3 的摄像头定义：
在代码中找到摄像头引脚部分，替换为：
```cpp
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
```

---

## Part 9：查看 On-Device Performance（设备端性能）

在 Edge Impulse 的 Deployment 页面，构建前会显示预估的设备端性能：

| 指标 | 目标值 | 说明 |
|------|--------|------|
| Inferencing time | < 500ms | 推理一次所需时间 |
| Peak RAM usage | < 512KB | ESP32S3 的 PSRAM 有 8MB，绰绰有余 |
| Flash usage | < 1MB | 模型大小，ESP32S3 有 8MB Flash |

如果 RAM 超标，回到 Part 5 选更小的模型（0.05 而不是 0.1），或者把输入分辨率降到 48×48。

---

## 常见问题

| 问题 | 解决方法 |
|------|----------|
| 上传照片失败 | 检查照片格式是否为 JPG/PNG；单张不超过 5MB |
| Feature explorer 里两类混在一起 | 数据质量不够，两只猫可能长得太像；增加数据量或检查标注是否正确 |
| 训练准确率高但测试准确率低 | 过拟合了；开启数据增强，或减少 training cycles |
| Arduino 编译报错 "out of memory" | 确认 PSRAM 已开启；选择 int8 量化模型 |
| 编译报错 "cam_hal: DMA overflow" | ESP32 Arduino Core 版本问题，尝试安装 2.0.x 版本的 Core |
| 推理结果两个类别都是 ~0.5 | 模型不确定，可能需要更多数据或照片角度不一致 |

---

## 检查清单

- [ ] Edge Impulse 账号创建完成
- [ ] cat_a 和 cat_b 照片上传完成，数量各 150+
- [ ] Impulse 配置完成：96×96 RGB + Transfer Learning
- [ ] 特征生成完成，Feature explorer 显示两类可分
- [ ] 模型训练完成，准确率 > 85%
- [ ] 测试集验证通过
- [ ] Arduino Library 下载并安装到 Arduino IDE
- [ ] 示例代码烧录到 ESP32S3 Sense，Serial Monitor 显示推理结果
- [ ] 推理结果正确（对着对应的猫，置信度 > 0.7）

**全部通过后，你就可以进入 Step 5：把猫咪识别和体重记录整合到一起了！**
