# STM32F103ZET6 CubeIDE 配置指南

本文档为 M1-M6 各阶段的 CubeMX 配置索引，每个阶段的详细配置步骤见 `milestones/` 目录。

---

## 配置阶段索引

| 阶段 | 配置范围 | 详细文档 |
| --- | --- | --- |
| M1 | 板级冒烟：GPIO、USART、I2C、ADC、RTC | [M1_板级冒烟/02_CubeMX配置.md](milestones/M1_板级冒烟/02_CubeMX配置.md) |
| M2 | 传感器驱动（待展开） | 待补充 |
| M3 | 算法验证（待展开） | 待补充 |
| M4 | FreeRTOS 集成（待展开） | 待补充 |
| M5 | 低功耗优化（待展开） | 待补充 |
| M6 | 数据上报（待展开） | 待补充 |

---

## 通用配置原则

### 1. 时钟树配置（所有阶段统一）

| 时钟项 | 配置 | 结果 |
| --- | --- | --- |
| HSE Input Frequency | 8 MHz | 外部晶体输入 |
| PLL Source Mux | HSE | PLL 使用外部时钟 |
| PLL Multiplier | ×9 | PLL 倍频系数 |
| System Clock Mux | PLLCLK | 系统时钟使用 PLL |
| AHB Prescaler | /1 | HCLK = 72 MHz |
| APB1 Prescaler | /2 | PCLK1 = 36 MHz（最大 36 MHz） |
| APB2 Prescaler | /1 | PCLK2 = 72 MHz |
| ADC Prescaler | /6 | ADC 时钟 = 12 MHz（最大 14 MHz） |

**验证结果**：SYSCLK = 72 MHz, HCLK = 72 MHz, PCLK1 = 36 MHz, PCLK2 = 72 MHz

### 2. 调试接口

| 配置项 | 值 | 说明 |
| --- | --- | --- |
| Debug | `JTAG (4 pins)` 或 `Serial Wire` | 根据 ST-Link 接口选择 |
| Timebase Source | `SysTick` (M1-M4) | M5 阶段前迁移到 TIM4 |

### 3. 代码生成规则

- **Application Structure**: `Basic`
- **Backup previously generated files**: 勾选
- **Keep User Code when re-generating**: 勾选
- **Delete previously generated files when not re-generated**: 勾选
- **HAL Settings - Set all free pins as analog**: 不勾选（保留未使用引脚为浮空）

---

## 引脚分配总览

详细引脚分配见 `hardware/引脚分配_暂定方案.md`，M1 阶段已固化的引脚：

| 功能 | 引脚 | 配置 |
| --- | --- | --- |
| LED0 | PB5 | GPIO_Output, Push-Pull, Low |
| TRACE_PIN | PD11 | GPIO_Output, Push-Pull, High Speed |
| USART1_TX | PA9 | USART1_TX |
| USART1_RX | PA10 | USART1_RX |
| I2C1_SCL | PB8 | I2C1_SCL（重映射） |
| I2C1_SDA | PB9 | I2C1_SDA（重映射） |
| MPU6050_INT | PE2 | GPIO_EXTI2, Rising Edge, Pull-down |
| DS18B20_DQ | PG9 | GPIO_Output, Open-Drain, Pull-up, High Speed |
| ACS712_VOUT | PA0 | ADC1_IN0 |

---

## 常见问题

### Q1: I2C1 引脚为何是 PB8/PB9 而不是 PB6/PB7？

**A**: 开发板上 PB6/PB7 有引脚冲突，必须使用 I2C1 重映射功能将 SCL/SDA 映射到 PB8/PB9。配置时需在引脚图中手动选择 PB8/PB9，生成的代码会自动包含 `__HAL_AFIO_REMAP_I2C1_ENABLE()`。

### Q2: ADC 采样时间应该设置多少？

**A**: M1 阶段配置为 `28.5 Cycles`（采样时间 = (28.5 + 12.5) / 12 MHz = 3.4 μs），相比默认的 `1.5 Cycles` 更稳定，适合高阻抗信号源（如 ACS712 分压电路）。

### Q3: RTC 时钟源如何配置？

**A**: 在 `RCC` 中启用 `LSE (Crystal/Ceramic Resonator)`，在 `Clock Configuration` 中确认 `RTC Clock Mux` 选择 `LSE`，频率为 32.768 kHz。

### Q4: 为何不使用 HSI 内部振荡器？

**A**: HSI 精度较低（±1%），不适合需要精确定时的应用。本项目使用外部 8 MHz HSE 晶体和 32.768 kHz LSE 晶体，确保系统时钟和 RTC 精度。

---

## 工具链版本

| 工具 | 推荐版本 | 备注 |
| --- | --- | --- |
| STM32CubeIDE | 待记录 | 集成开发环境 |
| arm-none-eabi-gcc | 待记录 | 交叉编译器 |
| HAL 库 | STM32Cube FW_F1 V1.8.3 | 固件库 |
| ST-Link 固件 | 待记录 | 调试器固件 |

**记录方式**：
- CubeIDE 版本：`Help → About STM32CubeIDE`
- GCC 版本：在终端执行 `arm-none-eabi-gcc --version`
- ST-Link 固件：在 CubeIDE 调试配置中查看

---

## 更新记录

- 2026-08-19：创建配置指南索引文档
