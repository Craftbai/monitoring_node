# STM32F103ZET6 STM32CubeIDE 完整配置教程

## 1. 教程目标与适用硬件

本教程用于在 STM32CubeIDE 中为以下目标建立新工程：

- 开发板：正点原子精英 STM32F103 开发板 V2。
- MCU：`STM32F103ZET6`，LQFP144，512 KB Flash，64 KB SRAM。
- 调试器：ST-Link，当前工程使用 `JTAG (4 pins)`；若探针只有两线接口再改为 SWD。
- HSE：8 MHz 外部晶体。
- LSE：32.768 kHz 板载 RTC 晶体。
- 首版功能：USART1 日志、板载 LED、测试 GPIO、RTC/Stop、SPI 振动、I2C 温度、ADC DMA 电流采样，后续启用 FreeRTOS。

引脚依据为[STM32F103ZET6 引脚分配](../hardware/引脚分配_暂定方案.md)。当前 M1-A 已完成代码生成和主机侧 Build；教程中的下载、GPIO/UART、RTC/Stop 和传感器步骤仍不代表目标板已经运行通过。

## 2. 推荐实施顺序

不要第一次生成工程时同时打开所有外设。按以下顺序配置和验证：

1. M1-A：工程、JTAG (4 pins)、HSE/LSE、72 MHz 时钟、USART1、PB5、PD11。
2. M1-B：RTC Alarm、Stop 模式与唤醒后时钟恢复。
3. M2-A：SPI1、ADXL345 片选和 PE2 中断。
4. M2-B：I2C1 与温度传感器。
5. M2-C：TIM3、ADC1_IN10 和 DMA。
6. M5：把 HAL 时基迁移到 TIM4，再启用 FreeRTOS。

每完成一个阶段都先 Build、下载和保存证据。前一阶段失败时，不继续叠加后面的外设。

## 3. 新建 STM32 工程

### 3.1 打开新建向导

1. 启动 STM32CubeIDE。
2. 选择 `File -> New -> STM32 Project`。
3. 选择 `MCU/MPU Selector`，不要选择 CMake Project。
4. 在 `Commercial Part Number` 中搜索 `STM32F103ZET6`。
5. 选择不带 `TR` 后缀的 `STM32F103ZET6`，点击 `Next`。

`TR` 只表示卷带包装，不改变芯片功能。不要选择 `STM32F103C8T6`，也不要通过文本编辑旧 `.ioc` 的 MCU 字段来换芯片。

### 3.2 工程信息

建议填写：

| 项目 | 建议值 |
| --- | --- |
| Project Name | `monitoring_node_f103ze` |
| Targeted Language | C |
| Targeted Binary Type | Executable |
| Targeted Project Type | `STM32Cube`，确保创建并保留 `.ioc` 配置文件 |
| Toolchain/IDE | STM32CubeIDE |

若要把工程直接放入当前仓库，最终路径应为：

```text
F:\Program\Embedded_Portfolio_Projects\03_monitoring_node\firmware\monitoring_node_f103ze
```

创建前确认目标目录不存在，避免把新工程叠加到旧工程目录。

### 3.3 固件包

| 项目 | 配置 |
| --- | --- |
| Firmware Package | `STM32Cube FW_F1 V1.8.3`（本机通过 V1.8.0 基础包 + V1.8.3 补丁离线安装） |
| Code Generator Options | `Copy only the necessary library files` |

该复制策略会把当前工程需要的 HAL/CMSIS 文件放入工程，避免依赖开发机上的绝对仓库引用，又不会复制整套未使用的驱动。

## 4. M1-A：最小板级配置

### 4.1 SYS

进入 `Pinout & Configuration -> System Core -> SYS`：

| 项目 | 值 | 原因 |
| --- | --- | --- |
| Debug | `JTAG (4 pins)` | 当前 ZET6 引脚充足，按精英板教程保留完整四线 JTAG；PB4/NJTRST 释放 |
| System Wake-Up | 不勾选 | PA0 是板载 KEY_UP/WK_UP，当前使用 RTC 唤醒 |
| Timebase Source | `SysTick` | 裸机阶段沿用 HAL 默认时基 |

JTAG 4 线占用：

| 信号 | MCU 引脚 |
| --- | --- |
| JTMS / TMS | PA13 |
| JTCK / TCK | PA14 |
| JTDI / TDI | PA15 |
| JTDO / TDO | PB3 |
| Reset | NRST |

JTAG (4 pins) 不使用 PB4/NJTRST，因此 PB4 可以释放。PA15 同时连接 ATK-MODULE LED，PB3 同时连接 CAMERA/OLED WEN；调试期间不要接入占用这两个信号的扩展模块。若实际 ST-Link 只有 SWDIO/SWCLK 两线接口，则应把 Debug 改为 `Serial Wire`，并按 SWD 接线。

### 4.2 RCC

进入 `System Core -> RCC`：

| 项目 | 值 |
| --- | --- |
| High Speed Clock (HSE) | `Crystal/Ceramic Resonator` |
| Low Speed Clock (LSE) | `Crystal/Ceramic Resonator` |
| Master Clock Output | 不启用 |

这里选择 Crystal/Ceramic 是因为板上连接无源晶体。只有外部电路直接向 OSC_IN 输入有源方波时才选择 Bypass。

### 4.3 72 MHz 时钟树

进入 `Clock Configuration`，按下表设置：

| 时钟项 | 配置 | 结果 |
| --- | --- | --- |
| HSE Input Frequency | 8 MHz | 外部晶体输入 |
| PLL Source Mux | HSE | PLL 使用外部时钟 |
| HSE Prescaler | `/1` | PLL 输入 8 MHz |
| PLL Mul | `×9` | PLLCLK 72 MHz |
| System Clock Mux | PLLCLK | SYSCLK 72 MHz |
| AHB Prescaler | `/1` | HCLK 72 MHz |
| APB1 Prescaler | `/2` | PCLK1 36 MHz |
| APB2 Prescaler | `/1` | PCLK2 72 MHz |
| ADC Prescaler | M1 暂时显示灰色 `/2 = 36 MHz` | ADC 尚未启用时只是不可编辑占位；M2 启用 ADC1 后改为 `/6`，得到 12 MHz |
| USB Prescaler | `/1.5` | 仅启用 USB 时使用 48 MHz |

正确结果应为：

```text
SYSCLK = 72 MHz
HCLK   = 72 MHz
PCLK1  = 36 MHz
APB1 Timer Clock = 72 MHz
PCLK2  = 72 MHz
APB2 Timer Clock = 72 MHz
ADC Clock = M1 未启用；M2 启用后为 12 MHz
```

APB1 分频为 `/2` 时，TIM2/TIM3/TIM4 的定时器输入时钟自动变为 PCLK1 的 2 倍，即 72 MHz。

### 4.4 USART1 日志串口

进入 `Connectivity -> USART1`：

| 项目 | 值 |
| --- | --- |
| Mode | `Asynchronous` |
| Baud Rate | `115200 Bits/s` |
| Word Length | `8 Bits` |
| Parity | `None` |
| Stop Bits | `1` |
| Data Direction | `Receive and Transmit` |
| Hardware Flow Control | `Disable` |
| Over Sampling | `16 Samples` |

CubeMX 应自动分配：

- PA9：`USART1_TX`。
- PA10：`USART1_RX`。

精英板通过 P3 跳线把 USART1 接到板载 CH340。确认 P3 跳线处于串口连接位置，串口终端设置为 `115200-8-N-1`、无流控。

### 4.5 GPIO

在 Pinout 视图中设置以下 GPIO：

| 引脚 | 功能 | User Label | 初始电平 | 模式 | Pull | Speed |
| --- | --- | --- | --- | --- | --- | --- |
| PB5 | 板载 LED0 | `STATUS_LED` | High | Output Push Pull | No Pull | Low |
| PD11 | 测试标记 | `TRACE_PIN` | Low | Output Push Pull | No Pull | Low |

PB5 的 LED0 常见为低有效，因此初始 High 通常表示熄灭；M1 首次下载必须用实际闪烁结果确认极性。PD11 在板级 IO 表中完全独立，可连接示波器或逻辑分析仪。

不要继续使用旧方案中的 PC13 和 PB12：PC13 不是精英板板载 LED，PB12 已连接板载 W25Q128 的片选 `F_CS`。

### 4.6 Project Manager

进入 `Project Manager`：

### Project

| 项目 | 建议值 |
| --- | --- |
| Project Name | `monitoring_node_f103ze` |
| Toolchain/IDE | STM32CubeIDE |
| Minimum Heap Size | 暂用默认；这是 C 运行库堆，不等同于 FreeRTOS Heap |
| Minimum Stack Size | 暂用默认，结合 map 文件和运行证据调整 |

### Code Generator

建议勾选：

- `Copy only the necessary library files`。
- `Keep User Code when re-generating`。
- `Backup previously generated files when re-generating`。
- `Generate peripheral initialization as a pair of '.c/.h' files per peripheral`。

不要把业务逻辑写在无 `USER CODE` 保护的 CubeMX 生成区内。

## 5. 生成、构建与 ST-Link 下载

### 5.1 生成代码

1. 按 `Ctrl+S` 保存 `.ioc`。
2. IDE 提示是否生成代码时选择 `Yes`；如果此前选择过 `No` 或没有弹窗，保持 `.ioc` 编辑器处于当前活动页面，选择顶部菜单 `Project（项目） -> Generate Code（生成代码）`。
3. 等待代码生成完成。若提示固件包缺失，先安装/选择 `STM32Cube FW_F1 V1.8.3`，再重新执行 Generate Code；离线安装时先导入 V1.8.0 基础包，再导入 V1.8.3 补丁。
4. 在 Project Explorer 中右键工程选择 `Refresh`，或按 `F5`。
5. 确认出现 `Core`、`Drivers`、`Core/Startup/startup_stm32f103zetx.s` 和根目录下的 `STM32F103ZETX_FLASH.ld`。
6. 点击锤子图标执行 Build；`Debug` 输出目录通常在第一次 Build 成功后才出现。

若工程中仍只有 `.ioc`，说明代码生成尚未成功。此时查看底部 `Console` 的生成日志，不要继续 Build；“Project has no explicit encoding set”只是文本编码警告，不会阻止 CubeMX 生成代码。

Build 成功只能证明主机工具链能生成 ELF，不代表硬件配置正确。

当前 `monitoring_node_f103ze` 已生成 `Core/`、`Drivers/`、启动文件和链接脚本，并构建出 `Debug/monitoring_node_f103ze.elf`。`arm-none-eabi-size` 结果为 text 5660 B、data 12 B、bss 1636 B；此结果仍未覆盖 ST-Link 下载和目标板运行。

### 5.2 ST-Link JTAG 接线

| ST-Link | 开发板 |
| --- | --- |
| TMS | PA13 / JTMS |
| TCK | PA14 / JTCK |
| TDI | PA15 / JTDI |
| TDO | PB3 / JTDO |
| NRST | NRST |
| VREF/3.3 V | 目标板 3.3 V 参考 |
| GND | GND |

目标板与 ST-Link 必须共地。若开发板已经由 USB 供电，不要在未确认电源路径时再由 ST-Link 3.3 V 给整板供电。

只有 SWDIO/SWCLK 引脚的 ST-Link 不具备完整 JTAG 四线连接；这种情况下把 CubeMX Debug 改为 `Serial Wire`，接 PA13、PA14、NRST、VREF 和 GND。

### 5.3 Debug 配置

1. 选择 `Run -> Debug Configurations`。
2. 新建或选择 `STM32 C/C++ Application`。
3. 在 Debugger 页面确认探针为 ST-Link、接口为 JTAG；若实际使用两线探针，则 CubeMX 和 Debugger 都改为 SWD。
4. 常规情况使用硬件复位；连接失败时再尝试 `Connect under reset`。
5. 点击 Debug，确认能停在 `main()`。

## 6. M1-A 最小测试代码

在 `main.c` 的用户代码区中先做 LED、串口和追踪脚测试。以下逻辑应放在 CubeMX 的 `USER CODE` 区域：

```c
static const uint8_t start_msg[] = "monitoring_node_f103ze boot\r\n";

HAL_UART_Transmit(&huart1, (uint8_t *)start_msg, sizeof(start_msg) - 1U, 100U);

while (1)
{
    HAL_GPIO_WritePin(TRACE_PIN_GPIO_Port, TRACE_PIN_Pin, GPIO_PIN_SET);
    HAL_GPIO_TogglePin(STATUS_LED_GPIO_Port, STATUS_LED_Pin);
    HAL_Delay(500U);
    HAL_GPIO_WritePin(TRACE_PIN_GPIO_Port, TRACE_PIN_Pin, GPIO_PIN_RESET);
    HAL_Delay(500U);
}
```

验收结果：

- 串口每次复位输出一次启动文本。
- LED0 每 500 ms 改变一次状态。
- PD11 能观察到与程序逻辑一致的高低电平。
- 连续复位和重新下载均能正常连接。

## 7. M1-B：RTC、Alarm 与 Stop

### 7.1 RTC 配置

进入 `Timers -> RTC`：

1. 勾选 `Activate Clock Source`。
2. RTC 时钟源选择 LSE 32.768 kHz。
3. 启用 Alarm A 和 RTC Alarm interrupt。
4. 在 `NVIC Settings` 中启用 RTC Alarm 中断。

M1 调试阶段先设置 5 秒左右的唤醒间隔，确认稳定后再改为配置样例中的 300 秒。STM32F1 RTC 的实际 Alarm 配置应基于当前计数值计算下一次闹钟，不要只在启动时设置一次固定值后假设它会自动周期重复。

### 7.2 进入 Stop 前后的顺序

推荐流程：

```text
设置下一次 RTC Alarm
-> 串口输出 prepare_stop
-> 等待串口发送完成
-> TRACE_PIN 拉低
-> 清理唤醒标志
-> 进入 Stop
-> RTC Alarm 唤醒
-> 重新执行 SystemClock_Config
-> 恢复必要外设
-> 输出 wake 日志
```

STM32F1 从 Stop 唤醒后系统时钟不会自动恢复为原来的 PLL 72 MHz，因此必须重新调用 `SystemClock_Config()`，再使用依赖主时钟的 USART、SPI、I2C、ADC 和定时器。

M1-B 验收：连续完成至少 20 次“运行 -> Stop -> RTC 唤醒”，保存串口日志和 PD11 波形。仅看到串口继续打印，不能单独证明芯片确实进入过 Stop。

## 8. M2-A：SPI1 与 ADXL345

传感器到货并确认支持 3.3 V SPI 后再启用。

### 8.1 SPI1

进入 `Connectivity -> SPI1`：

| 项目 | 值 |
| --- | --- |
| Mode | `Full-Duplex Master` |
| Hardware NSS Signal | `Disable`，软件控制 CS |
| Frame Format | Motorola |
| Data Size | 8 Bits |
| First Bit | MSB First |
| Baud Rate Prescaler | `/16`，SPI 时钟约 4.5 MHz |
| Clock Polarity | High |
| Clock Phase | 2 Edge |
| CRC Calculation | Disable |

以上对应常用 SPI Mode 3。ADXL345 也支持 Mode 0；驱动配置和 CubeMX 必须保持一致，不能一边使用 Mode 0 时序、一边按 Mode 3 调试。

CubeMX 应分配 PA5/SCK、PA6/MISO、PA7/MOSI。

### 8.2 片选和中断

| 引脚 | User Label | 配置 |
| --- | --- | --- |
| PA4 | `ADXL345_CS` | Output Push Pull、初始 High、No Pull、Low Speed |
| PE2 | `ADXL345_INT1` | External Interrupt Mode with Rising edge、No Pull |

在 `NVIC Settings` 中启用 `EXTI line2 interrupt`。ADXL345 默认中断常见为高有效推挽输出；最终边沿和 Pull 仍应按所购模块、电路和寄存器配置复核。

PA4 同时连接精英板 ATK-MODULE KEY 信号。使用 ADXL345 时不要插入占用该引脚的 ATK 模块。

M2-A 先只验证器件 ID、寄存器读写和超时，再打开数据就绪/FIFO 中断。

## 9. M2-B：I2C1 与温度传感器

进入 `Connectivity -> I2C1`：

| 项目 | 值 |
| --- | --- |
| I2C Speed Mode | Standard Mode |
| Clock Speed | 100000 Hz |
| Duty Cycle | 2 |
| Addressing Mode | 7-bit |
| Dual Address | Disable |
| General Call | Disable |
| Clock Stretching | Enable |

CubeMX 应分配 PB6/SCL、PB7/SDA。

精英板的 PB6/PB7 已连接板载 24C02，并带 4.7 kΩ 上拉。外接 TMP102 前注意：

1. 24C02 的 7 位地址通常为 `0x50`，外部器件地址不得冲突。
2. 模块若也带上拉，会与板载电阻并联；先测量等效阻值和波形。
3. 所有模块使用 3.3 V 兼容电平并共地。
4. 初次调试先扫描已知地址或读取固定寄存器，不直接接入任务系统。

## 10. M2-C：TIM3 触发 ADC1 + DMA

当前配置样例要求电流通道采样率为 1600 Hz。使用 TIM3 TRGO 触发 ADC1，避免通过软件延时产生不稳定采样间隔。

### 10.1 ADC1

在 Pinout 视图把 PC0 设置为 `ADC1_IN10`，再进入 `Analog -> ADC1`：

| 项目 | 值 |
| --- | --- |
| Scan Conversion Mode | Disable |
| Continuous Conversion Mode | Disable |
| Discontinuous Conversion Mode | Disable |
| External Trigger Conversion Source | Timer 3 Trigger Out event |
| Data Alignment | Right alignment |
| Number of Conversion | 1 |
| Rank 1 | Channel 10 |
| Sampling Time | 71.5 Cycles，后续按前端阻抗复核 |

启用 ADC1 后返回 `Clock Configuration`，此时原来灰色的 ADC Prescaler 会解锁。选择 `/6`，使 ADC 时钟为 12 MHz。M1 中显示的灰色 `/2 = 36 MHz` 没有实际送入未启用的 ADC；ADC 启用后不能继续使用 36 MHz。

PC0 同时连接 CAMERA/OLED 接口的 D0。使用电流 ADC 时不要插该摄像头/显示扩展模块，模拟前端输出必须限制在 `0 V` 到 `VDDA` 范围内。

### 10.2 TIM3 产生 1600 Hz TRGO

进入 `Timers -> TIM3`，设置内部时钟：

| 项目 | 值 |
| --- | --- |
| Clock Source | Internal Clock |
| Prescaler | `4499` |
| Counter Period | `9` |
| Counter Mode | Up |
| Trigger Output (TRGO) | Update Event |

APB1 Timer Clock 为 72 MHz，因此：

```text
72,000,000 / (4499 + 1) / (9 + 1) = 1600 Hz
```

### 10.3 DMA

在 ADC1 的 `DMA Settings` 中添加 DMA：

| 项目 | 值 |
| --- | --- |
| DMA Request | ADC1 / DMA1 Channel1 |
| Direction | Peripheral to Memory |
| Mode | Circular |
| Peripheral Increment | Disable |
| Memory Increment | Enable |
| Peripheral Data Width | Half Word |
| Memory Data Width | Half Word |
| Priority | High |

在 NVIC 中启用 DMA1 Channel1 中断。DMA 缓冲应使用 `uint16_t`，长度固定并同时处理半缓冲和全缓冲回调。

启动顺序建议为：

```c
HAL_ADCEx_Calibration_Start(&hadc1);
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buffer, ADC_BUFFER_LENGTH);
HAL_TIM_Base_Start(&htim3);
```

先校准 ADC，再启动 DMA，最后启动触发定时器。停止时采用相反顺序，避免 DMA 缓冲仍被外设写入。

## 11. 传感器电源使能

将 PE6 配置为：

| 项目 | 值 |
| --- | --- |
| User Label | `SENSOR_PWR_EN` |
| Initial Output Level | Low |
| GPIO Mode | Output Push Pull |
| Pull | No Pull |
| Speed | Low |

PE6 只能连接外部负载开关的 EN，不能直接给多个传感器供电。负载开关型号、有效电平和默认上下拉确定后，再决定 Low 是否表示关闭。

## 12. M5：启用 FreeRTOS

完成 UART、RTC/Stop、SPI、I2C 和 ADC DMA 的裸机验证后再启用 FreeRTOS。

### 12.1 迁移 HAL Timebase

先进入 `System Core -> SYS`，把：

```text
Timebase Source: SysTick -> TIM4
```

原因是 FreeRTOS 使用 SysTick 作为调度节拍，HAL 再独立占用 SysTick 容易形成时基职责混淆。TIM3 已用于 ADC 采样，因此 HAL Timebase 使用 TIM4。

### 12.2 启用内核

进入 `Middleware -> FreeRTOS`：

| 项目 | 初始建议 |
| --- | --- |
| Interface | CMSIS_V2 |
| Tick Rate | 1000 Hz |
| Memory Allocation | 先按 CubeMX 默认，后续统一静态化 |
| Default Task | 只保留最小任务用于证明调度运行 |

先验证一个任务、UART 日志和 RTC/Stop 协调，再按[任务与数据流](../docs/02_任务与数据流.md)逐步拆分采集、处理、周期、上报和健康任务。

### 12.3 中断优先级

启用 FreeRTOS 后，所有会调用 `...FromISR()` 的 DMA、EXTI 和外设中断必须处于允许调用 RTOS API 的优先级范围。若 CubeMX 生成的：

```text
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
```

则数值优先级 5～15 的中断可以按规则调用 FreeRTOS ISR API，0～4 不得调用。最终以生成的 `FreeRTOSConfig.h` 为准，不要只复制示例数值。

## 13. 最终引脚总表

| 功能 | 引脚 | CubeMX 配置 | 当前阶段 |
| --- | --- | --- | --- |
| JTAG TMS | PA13 | SYS JTAG (4 pins) | M1 |
| JTAG TCK | PA14 | SYS JTAG (4 pins) | M1 |
| JTAG TDI | PA15 | SYS JTAG (4 pins) | M1 |
| JTAG TDO | PB3 | SYS JTAG (4 pins) | M1 |
| USART1_TX | PA9 | USART1 Asynchronous | M1 |
| USART1_RX | PA10 | USART1 Asynchronous | M1 |
| LED0 | PB5 | GPIO Output，`STATUS_LED` | M1 |
| 测试标记 | PD11 | GPIO Output，`TRACE_PIN` | M1 |
| RTC LSE | PC14/PC15 | RCC LSE | M1 |
| ADXL345_CS | PA4 | GPIO Output | M2 |
| SPI1_SCK | PA5 | SPI1 | M2 |
| SPI1_MISO | PA6 | SPI1 | M2 |
| SPI1_MOSI | PA7 | SPI1 | M2 |
| ADXL345_INT1 | PE2 | EXTI2 | M2 |
| I2C1_SCL | PB6 | I2C1 | M2 |
| I2C1_SDA | PB7 | I2C1 | M2 |
| 电流 ADC | PC0 | ADC1_IN10 | M2 |
| 传感器电源 EN | PE6 | GPIO Output，`SENSOR_PWR_EN` | M2 |
| ADC 采样触发 | TIM3 | 1600 Hz TRGO | M2 |
| HAL Timebase | TIM4 | FreeRTOS 启用后使用 | M5 |

## 14. 每阶段检查清单

### 生成前

- [ ] MCU 是 STM32F103ZET6/LQFP144。
- [ ] HSE 为 8 MHz Crystal，LSE 为 Crystal。
- [ ] SYS Debug 是 JTAG (4 pins)，PA13/PA14/PA15/PB3 无外设冲突。
- [ ] 时钟树无红色错误，SYSCLK/HCLK 为 72 MHz。
- [ ] APB1 为 36 MHz、APB2 为 72 MHz；ADC 未启用时允许灰色显示 36 MHz。
- [ ] PB12 没有被配置成测试 GPIO。
- [ ] PC13 没有被误当成板载 LED。

### M1 验证

- [ ] 工程可生成并 Build 成功。
- [ ] ST-Link 能识别并下载。
- [ ] USART1 通过板载 CH340 输出 115200-8-N-1 日志。
- [ ] PB5 LED0 极性已实测。
- [ ] PD11 波形与程序一致。
- [ ] RTC Alarm 能连续唤醒至少 20 次。
- [ ] Stop 唤醒后重新配置 72 MHz 时钟。

### M2 验证

- [ ] ADXL345 能读取正确器件 ID，并验证错误 ID 和超时。
- [ ] I2C1 能访问板载 24C02 和外接温度传感器，地址无冲突。
- [ ] PC0 输入范围安全，ADC 校准完成。
- [ ] 启用 ADC1 后，ADC Prescaler 已改为 `/6`，ADC 时钟为 12 MHz。
- [ ] TIM3 TRGO 实测采样率为 1600 Hz。
- [ ] DMA 半/全缓冲顺序正确，无覆盖和丢块。

### FreeRTOS 验证

- [ ] HAL Timebase 已迁移到 TIM4。
- [ ] SysTick 只承担 RTOS 调度节拍。
- [ ] DMA/EXTI 中断优先级符合 `FreeRTOSConfig.h`。
- [ ] 任务栈高水位、堆余量和链接 map 已记录。
- [ ] Stop 前后的任务、时钟和外设恢复顺序已实测。

## 15. 常见错误

| 现象 | 优先检查 |
| --- | --- |
| ST-Link 找不到芯片 | JTAG 的 TMS/TCK/TDI/TDO、GND/NRST、探针接口模式、目标板供电、BOOT 跳线、Connect under reset |
| 串口无输出 | P3 跳线、COM 口、115200-8-N-1、PA9/PA10、是否共地 |
| LED 不亮或逻辑相反 | PB5 是否正确、LED0 是否低有效、初始电平 |
| LSE 启动失败 | PC14/PC15 是否被占用、晶振和负载电容、备份域配置 |
| Stop 后串口乱码 | 唤醒后没有重新执行 `SystemClock_Config()` |
| I2C 总线拉不高 | 外部模块电平错误、并联上拉过强、设备拉低、地址/接线错误 |
| SPI 读 ID 错误 | CS 初始电平、Mode 0/3、MISO/MOSI 接反、时钟过快 |
| ADC 数值不稳 | 前端阻抗、采样时间、VDDA、接地、输入越界、未校准 |
| ADC 采样率不对 | TIM3 输入时钟误按 36 MHz 计算、PSC/ARR、TRGO 未选 Update |
| 启用 RTOS 后卡死 | HAL Timebase、ISR 优先级、任务栈、在中断中调用阻塞 API |

## 16. 证据边界

完成 `.ioc` 配置和代码生成，只能证明配置已建立；Build 成功只证明开发机工具链可生成目标文件。只有完成 ST-Link 下载、串口/波形检查、传感器读写、ADC 输入安全验证和多周期 Stop 唤醒，才能分别声称对应目标板功能已通过。
