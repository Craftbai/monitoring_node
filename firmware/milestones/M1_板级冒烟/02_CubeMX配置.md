# M1 CubeMX 配置步骤

本文档记录 M1-A 阶段在 STM32CubeIDE 中使用 CubeMX 图形化配置工具创建工程的详细步骤。

---

## 1. 创建新工程

### 1.1 启动 CubeIDE

1. 打开 STM32CubeIDE
2. 选择 `File → New → STM32 Project`
3. 等待芯片数据库加载完成

### 1.2 选择 MCU

1. 在 `MCU/MPU Selector` 标签页中
2. 搜索框输入 `STM32F103ZET6`
3. 选择 LQFP144 封装
4. 点击 `Next`

### 1.3 配置工程

| 配置项 | 值 |
| --- | --- |
| Project Name | `monitoring_node_f103ze` |
| Location | `F:\Program\Embedded_Portfolio_Projects\03_monitoring_node\firmware\` |
| Targeted Language | `C` |
| Targeted Binary Type | `Executable` |
| Targeted Project Type | `STM32Cube` |
| Toolchain/IDE | `STM32CubeIDE` |

点击 `Finish`。

### 1.4 初始化代码生成器

弹出 `Initialize all peripherals with their default mode?` 对话框：
- 选择 `No`（手动配置外设，避免冲突）

---

## 2. 系统核心配置

### 2.1 SYS

进入 `Pinout & Configuration → System Core → SYS`：

| 配置项 | 值 | 说明 |
| --- | --- | --- |
| Debug | `JTAG (4 pins)` | PA13/PA14/PA15/PB3 + NRST |
| System Wake-Up | 不勾选 | 使用 RTC 唤醒，不使用 PA0/WKUP |
| Timebase Source | `SysTick` | 裸机阶段使用默认时基（M5 阶段前迁移到 TIM4） |

**JTAG 引脚占用**：
- PA13: JTMS
- PA14: JTCK
- PA15: JTDI（同时连接 ATK-MODULE LED，调试期间不接模块）
- PB3: JTDO（同时连接摄像头 WEN，调试期间不接模块）
- NRST: 复位

**注意**：若 ST-Link 只有 SWDIO/SWCLK 两线接口，应改为 `Serial Wire` 并按 SWD 接线。

### 2.2 RCC

进入 `System Core → RCC`：

| 配置项 | 值 | 说明 |
| --- | --- | --- |
| High Speed Clock (HSE) | `Crystal/Ceramic Resonator` | 8 MHz 无源晶体 |
| Low Speed Clock (LSE) | `Crystal/Ceramic Resonator` | 32.768 kHz RTC 晶体 |
| Master Clock Output | 不启用 | 不需要 MCO 输出 |

**注意**：只有外部有源方波输入时才选择 `Bypass`，开发板使用无源晶体，必须选择 `Crystal/Ceramic Resonator`。

### 2.3 时钟树配置

进入 `Clock Configuration` 标签页：

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

**验证结果**：
- SYSCLK = 72 MHz ✅
- HCLK = 72 MHz ✅
- PCLK1 = 36 MHz ✅
- PCLK2 = 72 MHz ✅
- ADC 时钟 = 12 MHz ✅

---

## 3. 外设配置

### 3.1 USART1（日志输出）

进入 `Connectivity → USART1`：

| 配置项 | 值 |
| --- | --- |
| Mode | `Asynchronous` |
| Basic Parameters - Baud Rate | `115200` |
| Basic Parameters - Word Length | `8 Bits` |
| Basic Parameters - Parity | `None` |
| Basic Parameters - Stop Bits | `1` |
| Hardware Flow Control | `Disable` |

**引脚自动分配**：
- TX: PA9
- RX: PA10

### 3.2 GPIO（LED0）

在引脚图中找到 PB5：
1. 右键点击 PB5
2. 选择 `GPIO_Output`
3. 在 `System Core → GPIO` 中配置：

| 配置项 | 值 |
| --- | --- |
| GPIO output level | `Low` |
| GPIO mode | `Output Push Pull` |
| GPIO Pull-up/Pull-down | `No pull-up and no pull-down` |
| Maximum output speed | `Low` |
| User Label | `LED0` |

### 3.3 GPIO（TRACE_PIN）

在引脚图中找到 PD11：
1. 右键点击 PD11
2. 选择 `GPIO_Output`
3. 在 `System Core → GPIO` 中配置：

| 配置项 | 值 |
| --- | --- |
| GPIO output level | `Low` |
| GPIO mode | `Output Push Pull` |
| GPIO Pull-up/Pull-down | `No pull-up and no pull-down` |
| Maximum output speed | `High` |
| User Label | `TRACE_PIN` |

**注意**：TRACE_PIN 用于示波器测试，速度设置为 `High`。

### 3.4 I2C1（传感器接口）

进入 `Connectivity → I2C1`：

| 配置项 | 值 |
| --- | --- |
| I2C | `I2C` |
| I2C Speed Mode | `Standard Mode` |
| I2C Clock Speed | `100000` Hz |
| Analog Filter | `Enable` |
| Rise Time | `300` ns |
| Fall Time | `300` ns |

**重要：引脚重映射**

默认引脚是 PB6/PB7，但开发板上这两个引脚有冲突，必须重映射到 PB8/PB9：

1. 在 `System Core → GPIO` 中：
   - 找到 PB6，确认其**未被 I2C1 占用**
   - 找到 PB7，确认其**未被 I2C1 占用**
2. 在引脚图中：
   - 右键点击 PB8，选择 `I2C1_SCL`
   - 右键点击 PB9，选择 `I2C1_SDA`
3. 验证引脚分配：
   - I2C1_SCL: PB8 ✅
   - I2C1_SDA: PB9 ✅

**User Label**：
- PB8: `MPU6050_SCL`
- PB9: `MPU6050_SDA`

### 3.5 GPIO（MPU6050 中断）

在引脚图中找到 PE2：
1. 右键点击 PE2
2. 选择 `GPIO_EXTI2`
3. 在 `System Core → GPIO` 中配置：

| 配置项 | 值 |
| --- | --- |
| GPIO mode | `External Interrupt Mode with Rising edge trigger detection` |
| GPIO Pull-up/Pull-down | `Pull-down` |
| User Label | `MPU6050_INT` |

4. 在 `System Core → NVIC` 中启用中断：
   - `EXTI line2 interrupt` → `Enabled`

### 3.6 GPIO（DS18B20 数据线）

在引脚图中找到 PG9：
1. 右键点击 PG9
2. 选择 `GPIO_Output`
3. 在 `System Core → GPIO` 中配置：

| 配置项 | 值 |
| --- | --- |
| GPIO output level | `High` |
| GPIO mode | `Output Open Drain` |
| GPIO Pull-up/Pull-down | `Pull-up` |
| Maximum output speed | `High` |
| User Label | `DS18B20_DQ` |

**注意**：1-Wire 协议需要开漏输出 + 上拉电阻。

### 3.7 ADC1（电流传感器）

进入 `Analog → ADC1`：

| 配置项 | 值 |
| --- | --- |
| Mode | 勾选 `IN0` |
| Configuration - Scan Conversion Mode | `Disable` |
| Configuration - Continuous Conversion Mode | `Disable` |
| Configuration - Discontinuous Conversion Mode | `Disable` |
| Configuration - DMA Continuous Requests | `Disable` |
| Number of Conversion | `1` |
| External Trigger Conversion Source | `Regular Conversion launched by software` |
| Rank1 - Channel | `Channel 0` |
| Rank1 - Sampling Time | `28.5 Cycles` |

**引脚自动分配**：
- IN0: PA0

**User Label**：
- PA0: `ACS712_VOUT`

**采样时间优化**：
- 默认 `1.5 Cycles` 过短，容易受干扰
- 改为 `28.5 Cycles`，采样时间 = (28.5 + 12.5) / 12 MHz = 3.4 μs

### 3.8 RTC（实时时钟）

进入 `Timers → RTC`：

| 配置项 | 值 |
| --- | --- |
| Activate Clock Source | 勾选 |
| Activate Calendar | 勾选 |
| RTC OUT | `Disable` |

**时钟源配置**：
- 在 `Clock Configuration` 中，确认 `RTC Clock Mux` 选择 `LSE`
- LSE 频率：32.768 kHz

**Alarm 配置**（用于 Stop 唤醒）：
进入 `Parameter Settings → Alarm`：
- `Alarm` → `Enable`
- `Alarm Mask` → `Alarm Mask None`（按实际需求配置，M1 阶段仅验证唤醒功能）

**NVIC 中断**：
- `RTC alarm interrupt through EXTI line` → `Enabled`

---

## 4. 代码生成配置

### 4.1 项目管理器

进入 `Project Manager` 标签页：

#### 4.1.1 Project Settings

| 配置项 | 值 |
| --- | --- |
| Application Structure | `Basic` |
| Do not generate the main() | 不勾选 |
| Generate peripheral initialization as a pair of '.c/.h' files per peripheral | 不勾选 |
| Backup previously generated files when re-generating | 勾选 |
| Keep User Code when re-generating | 勾选 |
| Delete previously generated files when not re-generated | 勾选 |

#### 4.1.2 Code Generator

| 配置项 | 值 |
| --- | --- |
| STM32Cube MCU packages and embedded software packs | `Copy only the necessary library files` |
| Generated files | `Generate peripheral initialization as a pair of '.c/.h' files per peripheral` → 不勾选（保持默认） |
| HAL Settings | `Set all free pins as analog` → 不勾选（保留未使用引脚为浮空） |

### 4.2 生成代码

1. 保存 `.ioc` 文件（Ctrl+S）
2. 点击 `GENERATE CODE` 按钮
3. 等待代码生成完成
4. 确认无错误或警告

---

## 5. 验证配置

### 5.1 检查生成的文件

生成后的目录结构：
```
firmware/monitoring_node_f103ze/
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   ├── stm32f1xx_hal_conf.h
│   │   ├── stm32f1xx_it.h
│   │   └── gpio.h
│   └── Src/
│       ├── main.c
│       ├── stm32f1xx_hal_msp.c
│       ├── stm32f1xx_it.c
│       └── system_stm32f1xx.c
├── Drivers/
│   ├── STM32F1xx_HAL_Driver/
│   └── CMSIS/
├── monitoring_node_f103ze.ioc
└── .project
```

### 5.2 编译工程

在 CubeIDE 中：
1. `Project → Build Project`（Ctrl+B）
2. 确认编译输出：
   - `0 errors, 0 warnings` ✅
   - 生成 `.elf` 和 `.bin` 文件 ✅

### 5.3 检查引脚分配

在 `main.h` 中确认宏定义：
```c
#define LED0_Pin GPIO_PIN_5
#define LED0_GPIO_Port GPIOB
#define TRACE_PIN_Pin GPIO_PIN_11
#define TRACE_PIN_GPIO_Port GPIOD
#define MPU6050_SCL_Pin GPIO_PIN_8
#define MPU6050_SCL_GPIO_Port GPIOB
#define MPU6050_SDA_Pin GPIO_PIN_9
#define MPU6050_SDA_GPIO_Port GPIOB
#define MPU6050_INT_Pin GPIO_PIN_2
#define MPU6050_INT_GPIO_Port GPIOE
#define DS18B20_DQ_Pin GPIO_PIN_9
#define DS18B20_DQ_GPIO_Port GPIOG
#define ACS712_VOUT_Pin GPIO_PIN_0
#define ACS712_VOUT_GPIO_Port GPIOA
```

### 5.4 检查时钟配置

在 `main.c` 的 `SystemClock_Config()` 函数中确认：
```c
RCC_OscInitStruct.HSEState = RCC_HSE_ON;
RCC_OscInitStruct.LSEState = RCC_LSE_ON;
RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;  // 8 MHz × 9 = 72 MHz

RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;   // HCLK = 72 MHz
RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;    // PCLK1 = 36 MHz
RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;    // PCLK2 = 72 MHz
```

### 5.5 检查 I2C 引脚重映射

在 `stm32f1xx_hal_msp.c` 的 `HAL_I2C_MspInit()` 函数中确认：
```c
/**I2C1 GPIO Configuration
PB8     ------> I2C1_SCL
PB9     ------> I2C1_SDA
*/
GPIO_InitStruct.Pin = MPU6050_SCL_Pin|MPU6050_SDA_Pin;
GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

__HAL_AFIO_REMAP_I2C1_ENABLE();  // 关键：I2C1 引脚重映射
```

**注意**：确认 `__HAL_AFIO_REMAP_I2C1_ENABLE()` 已生成，否则 I2C1 会使用默认的 PB6/PB7。

---

## 6. M1-A 完成标志

- [x] CubeMX 工程创建成功（STM32F103ZET6）
- [x] 时钟树配置正确（HSE 8 MHz、PLL ×9、LSE 32.768 kHz、SYSCLK 72 MHz）
- [x] 外设引脚分配符合硬件基线
- [x] I2C1 重映射到 PB8/PB9（非默认 PB6/PB7）
- [x] ADC 采样时间优化为 28.5 cycles
- [x] 代码生成无警告或错误
- [x] CubeIDE 编译通过（0 errors, 0 warnings）
- [x] 生成 `.elf` 和 `.bin` 文件

---

## 7. 下一步（M1-B）

M1-A 完成后，进入 M1-B 阶段：板级硬件验证
- 详见 `04_测试计划.md`

---

## 更新记录

- 2026-08-19：创建 M1 CubeMX 配置步骤文档
