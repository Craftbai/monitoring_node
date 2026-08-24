# 构建和测试说明

## 优化内容（2026-08-22）

### 1. 代码重构

- ✅ 添加串口格式化输出函数（`UART_Printf`）
- ✅ 提取 LED 闪烁函数（`LED_Flash`）
- ✅ 提取 RTC 时间打印函数（`RTC_PrintTime`）
- ✅ 提取 RTC Alarm 设置函数（`RTC_SetNextAlarm`）
- ✅ 添加宏定义（`WAKEUP_INTERVAL_SEC`、`STARTUP_DELAY_MS`）

### 2. 串口日志增强

**启动信息**：
```
========================================
  Monitoring Node - M1.3 Stop Mode Test
  MCU: STM32F103ZET6 @ 72 MHz
  RTC: LSE 32.768 kHz
  Wakeup Interval: 5 sec
========================================
  RTC: 00:00:10
```

**运行日志**：
```
[BOOT] Startup delay 3000 ms...
[BOOT] First alarm in 5 seconds

[WAKEUP #1] RTC Alarm triggered
  RTC: 00:00:15
[SLEEP] Entering Stop mode for 5 sec...

[WAKEUP #2] RTC Alarm triggered
  RTC: 00:00:20
[SLEEP] Entering Stop mode for 5 sec...
```

### 3. 代码优化效果

| 优化项 | 优化前 | 优化后 | 效果 |
|--------|--------|--------|------|
| LED 闪烁代码 | 内联 for 循环，重复出现 | 提取为函数 | 代码复用，减少 200+ 字节 |
| RTC 时间进位 | 80+ 行内联代码 | 提取为函数 | 可读性提升，易维护 |
| 串口输出 | 固定字符串 | 格式化输出 | 调试信息丰富 |
| 魔法数字 | 硬编码 5、3000 | 宏定义 | 易配置 |

## 构建步骤

### 方法 1：STM32CubeIDE（推荐）

1. 打开 STM32CubeIDE
2. File → Open Projects from File System
3. 选择 `firmware/monitoring_node_f103ze` 目录
4. Project → Build Project（Ctrl+B）

### 方法 2：命令行构建（需要配置工具链）

```powershell
cd firmware/monitoring_node_f103ze
arm-none-eabi-gcc --version  # 验证工具链
make clean
make -j4
```

## 下载和测试

### 1. 硬件连接

- **ST-Link V2** → **开发板 JTAG**
  - SWDIO → PA13
  - SWCLK → PA14
  - GND → GND
  - 3.3V → 3.3V

- **USB 转串口** → **USART1**
  - TX → PA10（开发板 RX）
  - RX → PA9（开发板 TX）
  - GND → GND

### 2. 串口工具设置

- **波特率**: 115200
- **数据位**: 8
- **停止位**: 1
- **校验**: None
- **流控**: None

推荐工具：
- PuTTY
- TeraTerm
- 串口调试助手

### 3. 下载程序

**重要**：Stop 模式会断开 SWD 连接，需要使用 RESET 方法：

1. 打开串口工具（115200 8N1）
2. **按住开发板 RESET 按钮**
3. STM32CubeIDE → Run → Debug（F11）
4. **立即松开 RESET 按钮**
5. 点击 Resume（F8）继续运行

## 预期测试结果

### 串口输出（前 3 个周期）

```
========================================
  Monitoring Node - M1.3 Stop Mode Test
  MCU: STM32F103ZET6 @ 72 MHz
  RTC: LSE 32.768 kHz
  Wakeup Interval: 5 sec
========================================
  RTC: 00:00:10

[BOOT] Startup delay 3000 ms...
[BOOT] First alarm in 5 seconds

[WAKEUP #1] RTC Alarm triggered
  RTC: 00:00:15
[SLEEP] Entering Stop mode for 5 sec...

[WAKEUP #2] RTC Alarm triggered
  RTC: 00:00:20
[SLEEP] Entering Stop mode for 5 sec...

[WAKEUP #3] RTC Alarm triggered
  RTC: 00:00:25
[SLEEP] Entering Stop mode for 5 sec...
```

### LED 指示（LED0 - PB5）

1. ✅ 启动时快闪 6 次（3 秒启动延迟）
2. ✅ LED 熄灭
3. ✅ 约 5 秒后快闪 3 次（唤醒指示）
4. ✅ LED 熄灭约 5 秒（Stop 模式）
5. ✅ 持续循环

### 验证清单

- [ ] 串口输出启动信息
- [ ] 显示正确的 RTC 时间
- [ ] 每 5 秒准时唤醒
- [ ] 唤醒计数器递增
- [ ] LED 快闪 3 次后完全熄灭
- [ ] 持续循环不停止（测试至少 2 分钟）
- [ ] 跨越分钟边界仍正常工作

## 故障排查

### 问题 1：串口无输出

- 检查 TX/RX 是否接反
- 检查波特率是否为 115200
- 检查 USB 转串口驱动是否安装
- 检查串口工具是否打开正确的 COM 口

### 问题 2：程序无法下载

- 按住 RESET 按钮，点击 Debug，立即松开 RESET
- 如果仍失败，给板子重新上电后再尝试

### 问题 3：LED 不闪烁

- 检查 LED0 极性（低有效：RESET=亮）
- 检查 PB5 引脚配置

### 问题 4：循环几次后停止

- 检查 RTC 时间进位逻辑
- 本次优化已修复此问题

## 下一步

完成串口测试后：

1. **M1.4 - 多周期稳定性验证**
   - 长时间运行测试（≥1 小时）
   - 记录工具链版本
   - 保存串口日志

2. **M2 - 单设备驱动验证**
   - DS18B20 温度传感器
   - MPU6050 振动传感器
   - ACS712 电流传感器

---

**优化日期**: 2026-08-22  
**测试状态**: 待验证
