# 重构代码修改清单 - 快速执行版

## 说明
这个文档包含所有需要修改的代码片段，你可以直接复制粘贴或使用查找替换。

---

## 修改 1: monitoring_tasks.c - 头文件 include

**位置**：文件顶部

**查找**：
```c
#include "monitoring_spi.h"
```

**替换为**：
```c
#include "monitoring_bus.h"
#include "monitoring_drivers.h"
```

---

## 修改 2: monitoring_tasks.c - 简化初始化代码

**位置**：`MonitoringTasks_Create()` 函数中，约第 967-977 行

**查找**（删除整段）：
```c
  /* ===== 第 1 步：初始化底层驱动（SPI/NRF24/IWDG） ===== */
  /* 必须在任务创建前完成，避免任务运行时驱动未就绪。
   * - MonitoringSpi_Init(): 初始化 SPI2 状态统计
   * - MonitoringNrf24_Bind(): 绑定 SPI 传输和 CSN/CE 控制回调
   * - MonitoringNrf24_Init(): 配置 NRF24 寄存器（无模块时返回 NOT_PRESENT）
   * - MonitoringHardwareWatchdog_Init(): 配置 IWDG（当前禁用，仅软件看门狗）
   */
  MonitoringSpi_Init();
  MonitoringNrf24_Bind(&nrf24_bus);
  (void)MonitoringNrf24_Init();  /* 无模块时不影响 UART 主链路 */
  MonitoringHardwareWatchdog_Init();
```

**替换为**：
```c
  /* ===== 第 1 步：初始化通信接口（SPI + I2C） ===== */
  /* 必须在任务创建前完成，避免任务运行时驱动未就绪。 */
  MonitoringBus_Init();

  /* ===== 第 2 步：初始化传感器和模块 ===== */
  /* 初始化顺序：温度(DS18B20) → 振动(MPU6050) → 电流(ADC) → 无线(NRF24) → 看门狗(IWDG) */
  MonitoringDrivers_Init();
```

---

## 修改 3: monitoring_tasks.c - 删除 nrf24_bus 定义

**位置**：`MonitoringTasks_Create()` 函数开始部分，约第 950-960 行

**查找**（删除整段）：
```c
  monitoring_nrf24_bus_t nrf24_bus = {
    .transfer = MonitoringSpi_Transfer,
    .chip_select = MonitoringTasks_NrfChipSelect,
    .chip_enable = MonitoringTasks_NrfChipEnable,
    .ready = 1U
  };
```

**说明**：这段代码已经移到 `monitoring_drivers.c`，可以直接删除。

---

## 修改 4: monitoring_tasks.c - 删除 MonitoringAcquisition_Init 调用

**位置**：约第 1200-1204 行

**查找**（删除整段，包括注释）：
```c
   * MonitoringAcquisition_Init(): ADC 校准、MPU6050 初始化
   */
  MonitoringAlerts_Reset();
  MonitoringAcquisition_Init();
```

**替换为**：
```c
   */
  MonitoringAlerts_Reset();
```

---

## 修改 5: monitoring_acquisition.h - 删除 Init 函数声明

**位置**：文件中部

**查找**（删除这一行）：
```c
void MonitoringAcquisition_Init(void);
```

---

## 修改 6: monitoring_acquisition.c - 删除 Init 函数实现

**位置**：约第 200-210 行

**查找**（删除整个函数）：
```c
void MonitoringAcquisition_Init(void)
{
#if MONITOR_CURRENT_SENSOR_ENABLED
  g_adc_calibrated = (HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK) ? 1U : 0U;
  g_adc_capture_active = 0U;
#endif
  g_mpu_ready = (MPU6050_Init() == MPU6050_OK) ? 1U : 0U;
}
```

**说明**：完全删除这个函数，初始化已移到 `MonitoringDrivers_Init()`。

---

## 修改 7: monitoring_acquisition.c - 删除 monitoring_nrf24.h include

**位置**：文件顶部

**查找**（删除这一行，如果存在）：
```c
#include "monitoring_nrf24.h"
```

**说明**：采集层不应该依赖无线模块。

---

## 修改 8: mpu6050.c - 修改头文件 include

**位置**：文件顶部

**在 `#include "mpu6050.h"` 之后添加**：
```c
#include "monitoring_bus.h"
```

---

## 修改 9: mpu6050.c - 替换所有 I2C 读操作

**使用 IDE 的查找替换功能**（Ctrl+H，限定在 mpu6050.c 文件）：

**第 1 步**：
- 查找：`HAL_I2C_Mem_Read(&hi2c1, MPU6050_ADDR, `
- 替换为：`MonitoringI2c_MemRead(MPU6050_ADDR, `
- 点击 "Replace All"

**第 2 步**：
- 查找：`, I2C_MEMADD_SIZE_8BIT, `
- 替换为：`, `
- 点击 "Replace All"（在 mpu6050.c 范围内）

---

## 修改 10: mpu6050.c - 替换所有 I2C 写操作

**使用 IDE 的查找替换功能**：

**第 1 步**：
- 查找：`HAL_I2C_Mem_Write(&hi2c1, MPU6050_ADDR, `
- 替换为：`MonitoringI2c_MemWrite(MPU6050_ADDR, `
- 点击 "Replace All"

**第 2 步**：如果还有 `I2C_MEMADD_SIZE_8BIT` 残留
- 查找：`, I2C_MEMADD_SIZE_8BIT,`
- 替换为：`,`
- 点击 "Replace All"

---

## 修改 11: 删除 monitoring_spi.h 和 monitoring_spi.c

**在 CubeIDE 项目浏览器中**：
1. 右键 `Core/Inc/monitoring_spi.h` → Delete → 勾选 "Delete from disk"
2. 右键 `Core/Src/monitoring_spi.c` → Delete → 勾选 "Delete from disk"

---

## ✅ 修改完成检查清单

完成上述所有修改后，检查：

- [ ] `monitoring_tasks.c` 已修改（include 和初始化代码）
- [ ] `monitoring_acquisition.h` 删除了 Init 声明
- [ ] `monitoring_acquisition.c` 删除了 Init 函数
- [ ] `mpu6050.c` 已使用 I2C 封装
- [ ] `monitoring_spi.h/c` 已删除

---

## 📝 编译验证

在 CubeIDE 中：
```
Project → Build Project
```

**预期结果**：0 errors, 0 warnings

**如果有错误**：
1. 记录错误信息
2. 检查是否有遗漏的修改
3. 回到这个对话告诉我错误信息

---

## 🎯 下一步

编译成功后，告诉我"编译通过"，我会继续生成文件重命名的脚本。
