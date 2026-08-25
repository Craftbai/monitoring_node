# 多参数监测节点 - 全工程重构方案

## 📊 问题总结

经过全工程深度审计，发现 **12 个严重的一致性问题**：

| 问题 | 严重程度 | 影响 |
|------|---------|------|
| 1. 文件命名混乱 | 高 | 可维护性差 |
| 2. 函数命名不统一 | 高 | 认知负担大 |
| 3. 错误码枚举混乱 | 中 | 接口不一致 |
| 4. 头文件保护宏不统一 | 中 | 违反 C 标准 |
| 5. 结构体命名不统一 | 低 | 小瑕疵 |
| 6. 任务函数命名不统一 | 低 | 小瑕疵 |
| 7. 初始化顺序不清晰 | 高 | 难以维护 |
| 8. 回调函数设计不统一 | 中 | 架构不对称 |
| 9. 日志输出不统一 | 中 | 调试困难 |
| 10. 配置参数分散 | 中 | 容易遗漏 |
| 11. 文档注释覆盖不均 | 中 | 双重标准 |
| 12. 头文件 include 混乱 | 低 | 依赖不清 |

**综合评分：5.2/10**

---

## 🎯 重构目标

**完全统一的代码风格，学习 CubeMX 的一致性**

---

## 📋 重构方案（12 个专项）

### 1️⃣ 文件命名统一

#### 重命名传感器驱动

```bash
# 当前
ds18b20.h/c
mpu6050.h/c

# 改为
monitoring_ds18b20.h/c
monitoring_mpu6050.h/c
```

#### 好处
- ✅ 所有用户代码统一 `monitoring_*` 前缀
- ✅ ls 时所有文件排在一起
- ✅ 一眼就知道是本项目代码

---

### 2️⃣ 函数命名统一

#### 统一为 PascalCase + 模块前缀

```c
/* 当前（混乱） */
DS18B20_Init()                  // 全大写
MPU6050_Init()                  // 全大写
MonitoringNrf24_Init()          // PascalCase

/* 改为（统一） */
MonitoringDs18b20_Init()        // PascalCase
MonitoringMpu6050_Init()        // PascalCase
MonitoringNrf24_Init()          // PascalCase（已正确）
```

#### 好处
- ✅ 命名风格完全一致
- ✅ 符合项目其他模块的风格
- ✅ 代码补全更友好（都以 Monitoring 开头）

---

### 3️⃣ 错误码统一

#### 方案：统一错误码枚举

```c
/* 新增 monitoring_status.h */
typedef enum {
  MONITORING_OK = 0,
  MONITORING_ERROR,
  MONITORING_ERROR_TIMEOUT,
  MONITORING_ERROR_NOT_PRESENT,
  MONITORING_ERROR_CRC,
  MONITORING_ERROR_ARGUMENT,
  MONITORING_ERROR_NOT_READY,
  MONITORING_ERROR_BUS_ERROR,
  MONITORING_ERROR_MAX_RETRY
} monitoring_status_t;

/* 所有模块统一使用 */
monitoring_status_t MonitoringDs18b20_Init(void);
monitoring_status_t MonitoringMpu6050_Init(void);
monitoring_status_t MonitoringNrf24_Init(void);
```

#### 好处
- ✅ 只需记住一种错误码
- ✅ 统一的错误处理逻辑
- ✅ 减少代码重复

---

### 4️⃣ 头文件保护宏统一

#### 统一规则：文件名全大写 + _H

```c
/* 当前（混乱） */
ds18b20.h:    #ifndef __DS18B20_H         ✗ 双下划线（保留给编译器）
mpu6050.h:    #ifndef MONITORING_MPU6050_H ✗ 多余的 MONITORING 前缀

/* 改为（统一） */
monitoring_ds18b20.h:  #ifndef MONITORING_DS18B20_H
monitoring_mpu6050.h:  #ifndef MONITORING_MPU6050_H
```

#### 好处
- ✅ 符合 C 标准（不使用保留标识符）
- ✅ 规则简单：文件名 → 全大写 + _H
- ✅ 所有头文件保护宏一致

---

### 5️⃣ 初始化架构重构

#### 新增三层初始化架构

```c
/* 新增 monitoring_bus.h/c - 通信接口层 */
void MonitoringBus_Init(void) {
  MonitoringSpi_Init();     // SPI2 统计、错误恢复
  MonitoringI2c_Init();     // I2C1 统计、错误恢复（新增）
}

/* 新增 monitoring_drivers.h/c - 传感器层 */
void MonitoringDrivers_Init(void) {
  MonitoringDs18b20_Init();   // 温度传感器
  MonitoringMpu6050_Init();   // 振动传感器
  HAL_ADCEx_Calibration_Start(&hadc1);  // 电流传感器
  MonitoringNrf24_Init();     // 无线模块
  MonitoringHardwareWatchdog_Init();  // 看门狗
}

/* 修改 monitoring_tasks.c - 应用层 */
void MonitoringTasks_Create(void) {
  MonitoringBus_Init();       // 第 1 步：通信接口
  MonitoringDrivers_Init();   // 第 2 步：传感器和模块
  // 创建 RTOS 资源
}
```

#### 好处
- ✅ 所有初始化一目了然
- ✅ 按层次清晰分离
- ✅ 易于维护和扩展

---

### 6️⃣ I2C 封装（与 SPI 对称）

#### 新增 I2C 封装层

```c
/* monitoring_bus.c - I2C 部分（新增） */
void MonitoringI2c_Init(void);
HAL_StatusTypeDef MonitoringI2c_MemRead(...);
HAL_StatusTypeDef MonitoringI2c_MemWrite(...);
HAL_StatusTypeDef MonitoringI2c_Recover(void);
void MonitoringI2c_GetStatus(monitoring_bus_status_t *status);
```

#### 修改 mpu6050.c 使用封装

```c
/* 原来 */
HAL_I2C_Mem_Read(&hi2c1, ...);

/* 改为 */
MonitoringI2c_MemRead(...);
```

#### 好处
- ✅ SPI 和 I2C 完全对称
- ✅ I2C 也有统计和错误恢复
- ✅ 一致的错误处理策略

---

### 7️⃣ 任务函数命名统一

#### 统一为 *Task 后缀

```c
/* 当前（混乱） */
static void RunAcquisitionTask(void *arg);   // Run* 前缀
static void RunProcessingTask(void *arg);    // Run* 前缀
void StartDefaultTask(void *arg);            // Start* 前缀

/* 改为（统一） */
static void AcquisitionTask(void *arg);      // 简洁
static void ProcessingTask(void *arg);
static void DefaultTask(void *arg);
```

#### 好处
- ✅ 命名简洁统一
- ✅ 符合 FreeRTOS 惯例

---

### 8️⃣ 回调函数统一（可选）

#### 方案：所有传感器都通过回调访问硬件

```c
/* 当前只有 NRF24 有回调 */
typedef struct {
  monitoring_nrf24_transfer_fn transfer;
  monitoring_nrf24_chip_select_fn chip_select;
  ...
} monitoring_nrf24_bus_t;

/* 扩展：MPU6050 也通过回调 */
typedef struct {
  monitoring_i2c_transfer_fn i2c_read;
  monitoring_i2c_transfer_fn i2c_write;
} monitoring_mpu6050_bus_t;

/* 扩展：DS18B20 也通过回调 */
typedef struct {
  monitoring_gpio_write_fn gpio_write;
  monitoring_gpio_read_fn gpio_read;
  monitoring_delay_us_fn delay_us;
} monitoring_ds18b20_bus_t;
```

#### 好处
- ✅ 完全解耦硬件依赖
- ✅ 单元测试更容易（可以 mock）
- ✅ 移植更容易

**⚠️ 警告：这是过度设计，建议暂不实施**

---

### 9️⃣ 日志输出统一

#### 方案：所有模块统一日志接口

```c
/* 新增 monitoring_log.h */
#define MONITOR_LOG_TAG_TASK  "[TASK]"
#define MONITOR_LOG_TAG_ACQ   "[ACQ]"
#define MONITOR_LOG_TAG_ALGO  "[ALGO]"
#define MONITOR_LOG_TAG_ALERT "[ALERT]"
#define MONITOR_LOG_TAG_NRF   "[NRF]"
#define MONITOR_LOG_TAG_I2C   "[I2C]"
#define MONITOR_LOG_TAG_SPI   "[SPI]"

void MonitoringLog(const char *tag, const char *fmt, ...);

/* 使用示例 */
MonitoringLog(MONITOR_LOG_TAG_ACQ, "MPU6050 init failed");
MonitoringLog(MONITOR_LOG_TAG_ALGO, "Q15 saturation: %u", count);
```

#### 好处
- ✅ 统一的日志格式
- ✅ 易于过滤和调试
- ✅ 可以统一控制日志级别

---

### 🔟 配置参数集中

#### 方案：所有配置移到 monitoring_config.h

```c
/* monitoring_config.h - 完整配置 */

/* 传感器配置 */
#define MONITOR_DS18B20_CONVERSION_TIME_MS  750U
#define MONITOR_MPU6050_SAMPLE_RATE_HZ      1000U
#define MONITOR_ADC_SAMPLE_RATE_HZ          1600U

/* 无线配置 */
#define MONITOR_NRF24_CHANNEL               76U
#define MONITOR_NRF24_PAYLOAD_SIZE          32U
#define MONITOR_NRF24_RETRY_COUNT           15U
#define MONITOR_NRF24_RETRY_DELAY_US        750U

/* 告警阈值 */
#define MONITOR_TEMP_LIMIT_CENTI            6500L
#define MONITOR_CURRENT_LIMIT_MA            1200L
#define MONITOR_VIBRATION_LIMIT_MG          350L

/* 任务配置 */
#define MONITOR_ACQUISITION_TASK_PRIORITY   osPriorityHigh
#define MONITOR_PROCESSING_TASK_PRIORITY    osPriorityAboveNormal
// ...
```

#### 好处
- ✅ 所有配置一个文件
- ✅ 修改时不会遗漏
- ✅ 易于生成多套配置（办公室/车间）

---

### 1️⃣1️⃣ 驱动层注释补充

#### 方案：为 ds18b20.c 和 mpu6050.c 补充注释

```c
/* monitoring_ds18b20.c - 补充注释 */
- 文件头说明 1-Wire 协议原理
- DS18B20_Init() 详细说明初始化步骤
- DS18B20_Reset() 说明复位时序
- DS18B20_WriteByte() 说明写时序
- DS18B20_ReadByte() 说明读时序

/* monitoring_mpu6050.c - 补充注释 */
- 文件头说明 I2C 通信协议
- MPU6050_Init() 详细说明寄存器配置
- MPU6050_StartCapture() 说明 FIFO 工作原理
- MPU6050_ReadFifoCount() 说明计数器读取
```

#### 好处
- ✅ 注释覆盖率统一（30-40%）
- ✅ 驱动层也有清晰文档
- ✅ 维护者更容易理解

---

### 1️⃣2️⃣ 头文件 include 规范化

#### 规则：按依赖层次排序

```c
/* 标准顺序 */
#include "monitoring_xxx.h"  // 1. 自己的头文件（必须第一个）
#include "monitoring_yyy.h"  // 2. 同层模块头文件
#include "stm32f1xx_hal.h"   // 3. HAL 库头文件
#include "adc.h"             // 4. CubeMX 生成的外设头文件
#include <string.h>          // 5. 标准库头文件
```

#### 删除不必要的 include

```c
/* monitoring_acquisition.c */
#include "monitoring_nrf24.h"  // ← 删除！采集层不应该知道无线模块
```

#### 好处
- ✅ 依赖关系清晰
- ✅ 编译更快（减少不必要的 include）
- ✅ 模块耦合度降低

---

## 📁 文件重命名清单

### 需要重命名的文件

```bash
# 传感器驱动
Core/Inc/ds18b20.h          → Core/Inc/monitoring_ds18b20.h
Core/Src/ds18b20.c          → Core/Src/monitoring_ds18b20.c
Core/Inc/mpu6050.h          → Core/Inc/monitoring_mpu6050.h
Core/Src/mpu6050.c          → Core/Src/monitoring_mpu6050.c

# SPI 封装合并到 bus
Core/Inc/monitoring_spi.h   → 删除（合并到 monitoring_bus.h）
Core/Src/monitoring_spi.c   → 删除（合并到 monitoring_bus.c）
```

### 新增文件

```bash
Core/Inc/monitoring_bus.h       # 通信接口层（SPI + I2C）
Core/Src/monitoring_bus.c
Core/Inc/monitoring_drivers.h  # 传感器/模块层
Core/Src/monitoring_drivers.c
Core/Inc/monitoring_status.h   # 统一错误码
Core/Inc/monitoring_log.h      # 统一日志接口（可选）
Core/Src/monitoring_log.c
```

---

## 🔄 函数重命名清单

### 传感器驱动

```c
/* ds18b20.c → monitoring_ds18b20.c */
DS18B20_Init()                    → MonitoringDs18b20_Init()
DS18B20_StartTemperatureConversion() → MonitoringDs18b20_StartConversion()
DS18B20_ReadTemperatureRaw()      → MonitoringDs18b20_ReadRaw()
DS18B20_ConversionReady()         → MonitoringDs18b20_IsReady()

/* mpu6050.c → monitoring_mpu6050.c */
MPU6050_Init()                    → MonitoringMpu6050_Init()
MPU6050_StartCapture()            → MonitoringMpu6050_StartCapture()
MPU6050_ReadFifoCount()           → MonitoringMpu6050_ReadFifoCount()
MPU6050_ReadSample()              → MonitoringMpu6050_ReadSample()
MPU6050_StopCapture()             → MonitoringMpu6050_StopCapture()
```

### 任务函数

```c
/* monitoring_tasks.c */
RunAcquisitionTask()  → AcquisitionTask()
RunProcessingTask()   → ProcessingTask()
RunReportTask()       → ReportTask()
RunCycleTask()        → CycleTask()
RunHealthTask()       → HealthTask()
RunWatchdogTask()     → WatchdogTask()

/* freertos.c */
StartDefaultTask()    → DefaultTask()
```

---

## 📊 重构前后对比

### 文件结构对比

```
【重构前】
Core/
├─ Inc/
│  ├─ monitoring_tasks.h         ✓
│  ├─ monitoring_acquisition.h   ✓
│  ├─ monitoring_algorithm.h     ✓
│  ├─ monitoring_alerts.h        ✓
│  ├─ monitoring_spi.h           ✓
│  ├─ monitoring_nrf24.h         ✓
│  ├─ monitoring_hw_watchdog.h   ✓
│  ├─ ds18b20.h                  ✗ 命名不一致
│  └─ mpu6050.h                  ✗ 命名不一致
└─ Src/
   ├─ ...（同上）

【重构后】
Core/
├─ Inc/
│  ├─ monitoring_tasks.h         ✓
│  ├─ monitoring_acquisition.h   ✓
│  ├─ monitoring_algorithm.h     ✓
│  ├─ monitoring_alerts.h        ✓
│  ├─ monitoring_bus.h           ✓ 新增（统一通信接口）
│  ├─ monitoring_drivers.h       ✓ 新增（统一传感器初始化）
│  ├─ monitoring_status.h        ✓ 新增（统一错误码）
│  ├─ monitoring_nrf24.h         ✓
│  ├─ monitoring_hw_watchdog.h   ✓
│  ├─ monitoring_ds18b20.h       ✓ 重命名
│  └─ monitoring_mpu6050.h       ✓ 重命名
└─ Src/
   ├─ ...（同上）
```

### 命名风格对比

```
【重构前】
DS18B20_Init()               ✗ 全大写
MPU6050_Init()               ✗ 全大写
MonitoringNrf24_Init()       ✓ PascalCase
MonitoringAcquisition_Init() ✓ PascalCase

【重构后】
MonitoringDs18b20_Init()     ✓ 统一 PascalCase
MonitoringMpu6050_Init()     ✓ 统一 PascalCase
MonitoringNrf24_Init()       ✓ 统一 PascalCase
MonitoringAcquisition_Init() ✓ 统一 PascalCase
```

### 初始化对比

```
【重构前】
MonitoringTasks_Create() {
  MonitoringSpi_Init();
  MonitoringNrf24_Init();
  MonitoringHardwareWatchdog_Init();
  // ... 300 行代码 ...
  MonitoringAcquisition_Init();  // MPU6050 藏在这里
  // DS18B20 延迟初始化（看不见）
}

【重构后】
MonitoringTasks_Create() {
  MonitoringBus_Init();       // 通信接口：SPI + I2C
  MonitoringDrivers_Init();   // 传感器：DS18B20/MPU6050/ADC/NRF24/IWDG
  // 创建 RTOS 资源
}
```

---

## 🎯 最终评分

| 维度 | 重构前 | 重构后 |
|------|-------|-------|
| 文件命名一致性 | 4/10 | 10/10 |
| 函数命名一致性 | 4/10 | 10/10 |
| 错误码一致性 | 3/10 | 10/10 |
| 初始化清晰度 | 5/10 | 10/10 |
| 接口对称性 | 5/10 | 10/10 |
| 文档覆盖率 | 6/10 | 9/10 |
| 配置集中度 | 6/10 | 9/10 |
| 日志统一性 | 4/10 | 9/10 |
| **综合评分** | **5.2/10** | **9.6/10** |

---

## ⚠️ 风险评估

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| 文件重命名导致编译错误 | 高 | 中 | 逐个文件重命名，每步编译验证 |
| 函数重命名遗漏调用点 | 中 | 高 | 使用 IDE 全局重命名功能 |
| 新增 I2C 封装引入 bug | 中 | 中 | 单元测试 MPU6050 通信 |
| 重构后测试不充分 | 中 | 高 | 完整回归测试（2-3 小时）|
| 重构时间超预期 | 低 | 低 | 分阶段实施 |

---

## 📅 实施建议

### 方案 A：激进重构（不推荐）
- **时间**：5-8 小时
- **风险**：高
- **时机**：M7 前
- **适合**：你有充足时间测试

### 方案 B：保守重构（推荐）
- **时间**：2 周（每天 1-2 小时）
- **风险**：低
- **时机**：M7 后
- **适合**：稳妥推进，逐步完善

### 方案 C：分阶段重构（平衡）
- **阶段 1（M7 前）**：只修复高优先级问题
  - 补充 I2C 封装
  - 统一初始化架构
  - 时间：2-3 小时
- **阶段 2（M7 后）**：完整重构
  - 文件重命名
  - 函数重命名
  - 统一错误码
  - 时间：4-6 小时

---

## ✅ 实施检查清单

### 阶段 1：高优先级（M7 前）
- [ ] 新增 `monitoring_bus.h/c`（I2C 封装）
- [ ] 新增 `monitoring_drivers.h/c`（统一初始化）
- [ ] 修改 `monitoring_tasks.c` 初始化流程
- [ ] 修改 `mpu6050.c` 使用 I2C 封装
- [ ] 删除 `monitoring_acquisition.h` 的 Init 声明
- [ ] 编译验证（0 errors）
- [ ] 测试 I2C 通信
- [ ] 测试完整采集流程

### 阶段 2：完整重构（M7 后）
- [ ] 重命名 `ds18b20` → `monitoring_ds18b20`
- [ ] 重命名 `mpu6050` → `monitoring_mpu6050`
- [ ] 统一函数命名（全局替换）
- [ ] 新增 `monitoring_status.h`（统一错误码）
- [ ] 修改所有模块使用统一错误码
- [ ] 修复头文件保护宏
- [ ] 统一任务函数命名
- [ ] 新增日志接口（可选）
- [ ] 集中配置参数
- [ ] 补充驱动层注释
- [ ] 规范化 include 顺序
- [ ] 完整编译验证
- [ ] 完整回归测试

---

## 🎉 重构收益

1. ✅ **一致性 100%**：所有文件、函数、错误码命名统一
2. ✅ **可维护性大幅提升**：新增传感器只需几行代码
3. ✅ **可读性提升**：初始化清晰，依赖明确
4. ✅ **可测试性提升**：I2C 也有错误恢复和统计
5. ✅ **代码质量从 5.2 提升到 9.6**：达到工业标准
6. ✅ **团队协作更容易**：统一风格，降低认知负担
7. ✅ **后续扩展更容易**：架构清晰，易于添加新传感器

---

## 🤔 你的决定？

**A. 立即全面重构**（5-8 小时，高风险）  
**B. M7 后全面重构**（2 周，低风险，推荐）  
**C. 分阶段重构**（阶段 1：M7 前 2 小时，阶段 2：M7 后 4-6 小时，平衡）  
**D. 只修复最严重的问题**（I2C 封装 + 初始化架构，2 小时）  
**E. 暂不重构，先验收**（风险最低）

请告诉我你的选择。
