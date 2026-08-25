# 函数名和类型名统一映射表

## 说明
这个文档列出了所有需要重命名的函数和类型。由于数量较多，建议使用 IDE 的全局查找替换功能。

---

## DS18B20 函数和类型重命名

### 执行步骤（在 CubeIDE 中）
1. 打开 Search → File Search (Ctrl+H)
2. Scope 选择 "Workspace"
3. 按照下表逐个替换

### 重命名清单

| 查找 | 替换为 | 说明 |
|------|--------|------|
| `ds18b20_status_t` | `monitoring_status_t` | 统一错误码类型 |
| `ds18b20_resolution_t` | `ds18b20_resolution_t` | 保持不变（内部枚举） |
| `DS18B20_OK` | `MONITORING_OK` | 成功状态 |
| `DS18B20_ERROR_NOT_PRESENT` | `MONITORING_ERROR_NOT_PRESENT` | 设备不存在 |
| `DS18B20_ERROR_TIMEOUT` | `MONITORING_ERROR_TIMEOUT` | 超时错误 |
| `DS18B20_ERROR_CRC` | `MONITORING_ERROR_CRC` | CRC 错误 |

**注意**：DS18B20 的函数名（DS18B20_Init 等）**暂时保持不变**，因为：
1. 这些函数在 monitoring_ds18b20.c 内部使用
2. 外部主要通过 monitoring_drivers.c 调用
3. 修改函数名需要同时修改函数定义、声明和所有调用点（工作量很大）
4. 当前的命名虽然不是 PascalCase，但清晰易懂

---

## MPU6050 函数和类型重命名

### 重命名清单

| 查找 | 替换为 | 说明 |
|------|--------|------|
| `mpu6050_status_t` | `monitoring_status_t` | 统一错误码类型 |
| `mpu6050_sample_t` | `mpu6050_sample_t` | 保持不变（内部结构体） |
| `MPU6050_OK` | `MONITORING_OK` | 成功状态 |
| `MPU6050_ERROR` | `MONITORING_ERROR` | 通用错误 |
| `MPU6050_ERROR_ARGUMENT` | `MONITORING_ERROR_ARGUMENT` | 参数错误 |
| `MPU6050_ERROR_BUS` | `MONITORING_ERROR_BUS` | 总线错误 |
| `MPU6050_ERROR_ID` | `MONITORING_ERROR_NOT_PRESENT` | ID 错误（映射为设备不存在） |
| `MPU6050_ERROR_FIFO` | `MONITORING_ERROR` | FIFO 错误（映射为通用错误） |
| `MPU6050_ERROR_TIMEOUT` | `MONITORING_ERROR_TIMEOUT` | 超时错误 |

**注意**：MPU6050 的函数名（MPU6050_Init 等）**暂时保持不变**，理由同上。

---

## 为什么不重命名函数名？

### 成本收益分析

**成本**：
- DS18B20 有 ~10 个公开函数
- MPU6050 有 ~5 个公开函数
- 每个函数需要修改：
  - 头文件中的声明
  - 源文件中的定义
  - 所有调用点（约 20-30 处）
- 总工作量：1-2 小时
- 风险：容易遗漏某些调用点

**收益**：
- 命名风格更统一
- 但功能完全不受影响

### 当前方案

**文件名**已经统一：
- ✅ `monitoring_ds18b20.h/c`
- ✅ `monitoring_mpu6050.h/c`

**错误码**即将统一：
- ✅ 使用 `monitoring_status_t`

**函数名**暂时保持：
- ⚠️ `DS18B20_Init()` 而不是 `MonitoringDs18b20_Init()`
- ⚠️ `MPU6050_Init()` 而不是 `MonitoringMpu6050_Init()`

这是一个**合理的折中方案**：
- 核心架构已经完全统一
- 文件名已经统一
- 函数名虽然不完美，但清晰易懂
- 可以在未来有时间时再慢慢重构

---

## 实施建议

### 方案 A：只统一错误码（推荐，10 分钟）
- 替换上述错误码枚举值
- 编译验证
- 提交

### 方案 B：完全统一函数名（1-2 小时）
- 需要修改所有函数声明、定义、调用
- 工作量大，容易出错
- 不建议在 M7 前进行

---

## 下一步

建议执行**方案 A**（只统一错误码）。

请告诉我你的决定：
- **A：只统一错误码**（快速，低风险）
- **B：完全统一函数名**（彻底，但耗时）
