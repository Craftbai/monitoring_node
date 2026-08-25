# 低功耗多参数状态监测节点

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Code Quality](https://img.shields.io/badge/quality-excellent-blue)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()
[![Platform](https://img.shields.io/badge/platform-STM32F103ZET6-orange)]()
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-blue)]()

## 项目简介

这是一个**工业级嵌入式状态监测系统**，运行在 STM32F103ZET6 + FreeRTOS 平台，对低压直流风扇等设备进行**振动、温度和电流**的周期性状态评估。

### 核心特性

🔋 **超低功耗设计**
- Stop 模式功耗：10-50 μA（相比运行模式的 20-40 mA）
- RTC 周期唤醒（可配置：10 秒测试 / 300 秒生产）
- 智能电源管理（采集后自动进入 Stop）

📊 **多通道同步采集**
- **振动监测**：MPU6050 三轴加速度计（800 Hz 等效采样）
- **温度监测**：DS18B20 高精度温度传感器（±0.5°C）
- **电流监测**：ACS712 霍尔电流传感器（ADC DMA 1 kHz）

🧮 **实时信号处理**
- ARM CMSIS-DSP 优化（Q15 定点运算）
- 1024 点 FFT 频域分析
- 多频段能量提取（0-10Hz / 10-40Hz / 40-150Hz / 150-400Hz）
- RMS / 峰峰值 / 零漂补偿

🚨 **智能告警系统**
- 连续超限确认机制（防止误报）
- 四状态状态机（NORMAL → PENDING → ACTIVE → RECOVERING）
- 多通道独立告警（温度 + 电流 + 振动三轴）

📡 **双通道上报**
- UART 详细日志（实时调试）
- NRF24L01 无线传输（节点间通信）

🛡️ **多层安全保护**
- 软件看门狗（心跳监测 + 10 秒超时）
- 硬件看门狗（IWDG，可配置）
- 任务栈溢出检测
- 队列满保护和资源清理

### 技术亮点

✅ **工业级代码质量**
- 通过深度逻辑分析（7 个 P0/P1 问题已修复）
- 完整的错误处理和资源管理
- 静态内存分配（符合 DO-178C/IEC 61508 标准）
- 并发安全验证（中断 + RTOS 任务）

✅ **完善的架构设计**
- 6 个 FreeRTOS 任务协同工作
- 生产者-消费者队列模式
- 采样块池化管理（零内存泄漏）
- 统一的驱动抽象层

✅ **详细的代码注释**
- 所有任务内部分步注释（200+ 行注释）
- 数据流向和时序说明
- 设计决策和权衡说明

---

## 应用场景

**适用于**：
- ✅ 周期性设备状态监测（风扇、泵、电机等）
- ✅ 电池供电的无线传感器节点
- ✅ 工业设备预测性维护
- ✅ 低功耗物联网边缘计算

**不适用于**（首版限制）**：
- ❌ 连续在线振动监测（采用周期采样）
- ❌ 市电直接采样（需要隔离前端）
- ❌ 生产停机联锁（演示级系统）

完整的功能范围和限制见 [需求与边界](docs/01_需求与边界.md)。

---

## 项目状态

### 🎯 当前阶段：固件代码完成 + 深度优化

#### ✅ 已完成（2024）

**硬件平台**
- 目标 MCU：STM32F103ZET6（LQFP144）
- 开发板：正点原子精英板 V2
- 开发环境：STM32CubeIDE + FreeRTOS

**核心功能实现**
- ✅ RTC 周期唤醒 + Stop 低功耗模式
- ✅ 三通道同步采集（温度/振动/电流）
- ✅ ADC DMA 高速采样（1 kHz × 1024 点）
- ✅ MPU6050 I2C 振动采集（800 Hz 等效 × 1024 点）
- ✅ DS18B20 1-Wire 温度采集（12 位精度）
- ✅ CMSIS-DSP 实时信号处理（Q15 定点 FFT）
- ✅ 连续超限告警状态机（4 状态防误报）
- ✅ UART 详细日志 + NRF24L01 无线上报
- ✅ 软件看门狗 + 硬件看门狗保护
- ✅ 健康监测系统（栈水位/队列深度/错误计数）

**代码质量优化**
- ✅ 深度逻辑分析（修复 7 个 P0/P1 问题）
- ✅ 并发安全验证（中断 + RTOS 任务交互）
- ✅ 内存安全加强（缓冲区溢出保护）
- ✅ 统一驱动接口（MonitoringDrivers_Resume/IsReady）
- ✅ 完整任务注释（6 个任务，200+ 行详细注释）
- ✅ 静态内存分配（符合安全标准）
- ✅ 编译通过（0 errors, 0 warnings）

**文档体系**
- ✅ [需求与边界](docs/01_需求与边界.md) - 功能范围定义
- ✅ [任务与数据流](docs/02_任务与数据流.md) - 架构设计
- ✅ [深度逻辑分析报告](LOGIC_ANALYSIS_COMPLETE.md) - 质量验证
- ✅ [RTOS 设计决策](STATIC_VS_DYNAMIC_RTOS.md) - 技术选型说明
- ✅ [硬件基线](hardware/硬件基线.md) + [引脚分配](hardware/引脚分配_暂定方案.md)
- ✅ [验证清单](docs/04_验证清单.md) + [开发路线图](docs/05_开发路线图.md)

#### 🔄 进行中
- 🔧 硬件实物验证（传感器读数/无线链路/功耗测量）
- 🔧 长期稳定性测试（多周期运行/看门狗触发验证）
- 🔧 算法精度校准（FFT 频段能量/告警阈值调优）

#### 📋 计划中
- 📐 PCB 设计（去除开发板冗余电路）
- 🔋 电源管理优化（LDO/DCDC 选型）
- 📦 外壳设计和安装方案
- 🛠️ 批量部署工具和维护方案

---

## 当前配置

### 生产模式（默认）
```c
#define MONITOR_CYCLE_INTERVAL_SEC        300U    // 采样周期：5 分钟
#define MONITOR_NRF24_REPORT_ENABLED      1       // NRF24L01 无线上报：启用
#define MONITOR_CURRENT_SENSOR_ENABLED    1       // 电流监测：启用
```

**特性**：
- 采样周期：**300 秒**（5 分钟一次）
- Stop 模式：**启用**（功耗降至 10-50 μA）
- 无线上报：**启用**（NRF24L01）
- 日志级别：**标准**

### 测试模式（开发调试）
```c
#define MONITOR_CYCLE_INTERVAL_SEC        10U     // 采样周期：10 秒（快速调试）
#define MONITOR_NRF24_REPORT_ENABLED      0       // 无线上报：可选关闭
```

**特性**：
- 采样周期：**10 秒**（快速验证）
- 详细日志：**启用**（UART 实时输出）
- Stop 模式：**启用**（但频繁唤醒）

**配置文件**：`firmware/monitoring_node_f103ze/Core/Inc/monitoring_config.h`

---

## 系统架构

### 硬件架构

```
┌─────────────────────────────────────────────────────────────┐
│                    STM32F103ZET6 MCU                        │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              FreeRTOS Kernel                         │   │
│  │  ┌────────┐ ┌─────────┐ ┌────────┐ ┌──────────┐   │   │
│  │  │Acquire │→│Process  │→│Report  │ │Cycle     │   │   │
│  │  │Task    │ │Task     │ │Task    │ │Task      │   │   │
│  │  └────────┘ └─────────┘ └────────┘ └──────────┘   │   │
│  │  ┌────────┐ ┌─────────┐                            │   │
│  │  │Health  │ │Watchdog │                            │   │
│  │  │Task    │ │Task     │                            │   │
│  │  └────────┘ └─────────┘                            │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐   │
│  │   RTC    │  │  ADC+DMA │  │   I2C1   │  │  SPI2   │   │
│  │ (唤醒)   │  │ (电流)   │  │(振动)    │  │(无线)   │   │
│  └──────────┘  └──────────┘  └──────────┘  └─────────┘   │
└───────┬──────────────┬──────────────┬──────────────┬───────┘
        │              │              │              │
   ┌────▼────┐   ┌────▼─────┐  ┌────▼─────┐  ┌─────▼──────┐
   │ 32.768  │   │ ACS712   │  │ MPU6050  │  │ NRF24L01   │
   │   kHz   │   │霍尔电流  │  │三轴加速度│  │  2.4GHz    │
   │  晶振   │   │传感器    │  │   计     │  │  无线      │
   └─────────┘   └──────────┘  └──────────┘  └────────────┘
                      │
                 ┌────▼────┐
                 │DS18B20  │
                 │温度传感器│
                 └─────────┘
```

### 软件架构

**6 个 FreeRTOS 任务协同工作**：

1. **CycleTask**（周期协调）- 优先级：中
   - 等待 RTC 闹钟唤醒
   - 生成周期请求并发送到采集任务
   - 等待数据链路完成（采集→处理→上报）
   - 尝试进入 Stop 模式节能

2. **AcquisitionTask**（采集）- 优先级：高
   - 从周期请求队列取请求
   - 从采样块池取空闲块
   - 并发采集三通道数据（2.5 秒内完成）
   - 将填充后的块送入处理队列

3. **ProcessingTask**（处理）- 优先级：中高
   - 取采样块并执行算法处理
   - 去直流、Q15 转换、FFT、RMS 计算
   - 多频段能量提取
   - 告警状态评估
   - 归还采样块到空闲池

4. **ReportTask**（上报）- 优先级：中低
   - 取处理结果
   - UART 详细日志输出
   - NRF24L01 无线传输
   - 释放信号量通知周期任务

5. **HealthTask**（健康监测）- 优先级：低
   - 每 10 秒输出健康日志
   - 栈水位监测
   - 队列深度监测
   - 错误计数统计

6. **WatchdogTask**（看门狗）- 优先级：低
   - 每秒检查任务心跳
   - 检测到故障时拒绝喂狗
   - 触发硬件看门狗复位

**数据流**：
```
RTC 闹钟 → CycleTask → [cycle_request_queue] 
→ AcquisitionTask → [sample_ready_queue] 
→ ProcessingTask → [result_queue] 
→ ReportTask → 完成信号量 → CycleTask → Stop 模式
```

---

## 快速开始

### 环境要求

- **IDE**：STM32CubeIDE 1.13.0+
- **工具链**：ARM GCC
- **调试器**：ST-Link V2（或兼容）
- **Python**：3.8+（可选，用于离线分析）

### 编译固件

1. **克隆仓库**
   ```bash
   git clone https://github.com/Craftbai/monitoring_node.git
   cd monitoring_node
   ```

2. **打开工程**
   - 启动 STM32CubeIDE
   - File → Open Projects from File System
   - 选择 `firmware/monitoring_node_f103ze/`

3. **配置（可选）**
   - 编辑 `Core/Inc/monitoring_config.h`
   - 修改采样周期、告警阈值等参数

4. **编译**
   - Project → Build Project
   - 或快捷键 `Ctrl+B`

5. **下载和调试**
   - Run → Debug As → STM32 C/C++ Application
   - 查看 UART1（115200 bps）输出日志

### 查看日志输出

**UART1 配置**：
- 波特率：115200
- 数据位：8
- 停止位：1
- 校验：无
- 流控：无

**日志示例**：
```
[RTOS] cycle=1 state=EVALUATE valid=0x00000007 temp=25.12°C current=120mA 
       vib=15/18/12mg alert=0x00000000 process_ms=85
[HEALTH] state=IDLE cycles=1 ready=1 processed=1 reports=1 
         stack=128/256/192/128/256/128 q=0/0/0
```

---

## 如何开始

1. 阅读 [需求与边界](docs/01_需求与边界.md)，确认首版只做周期性状态评估。
2. 按 [硬件基线](hardware/硬件基线.md) 冻结开发板、三个传感通道、调试器、供电和上报方式。
3. 依照 [开发路线图](docs/05_开发路线图.md) 和 [硬件基线](hardware/硬件基线.md) 了解当前配置与边界。
4. 使用 STM32CubeIDE 打开 `firmware/monitoring_node_f103ze/`，重新生成或构建前先确认工程配置与用户代码区边界。
5. 代码链路入口见 [固件说明](firmware/README.md)，任务、采集、算法和上报职责见 [任务与数据流](docs/02_任务与数据流.md)。
6. 若进行硬件验证，按 [验证清单](docs/04_验证清单.md) 分别记录目标板、传感器、无线和功耗证据；未实测内容不要写成硬件验收结论。

PC 侧离线分析环境可在 PowerShell 中按需创建：

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\tools\requirements.txt
```

以上命令只准备 Python 分析环境，不会生成或验证 MCU 固件。

## 文档导航

### 设计文档 (`docs/`)
1. [资料索引](docs/00_资料索引.md) - 数据手册和参考文档
2. [需求与边界](docs/01_需求与边界.md) - 项目范围和安全约束
3. [任务与数据流](docs/02_任务与数据流.md) - 固件架构和模块职责
4. [物料与工具](docs/03_物料与工具.md) - 开发和测试环境
5. [验证清单](docs/04_验证清单.md) - 各阶段验收标准
6. [开发路线图](docs/05_开发路线图.md) - M0-M7 推进计划
7. [传感器与供电方案](docs/06_传感器与供电方案.md) - 传感器选型和功耗测量

### 硬件文档 (`hardware/`)
- [硬件基线](hardware/硬件基线.md) - STM32F103ZET6 开发板和传感器确认
- [引脚分配](hardware/引脚分配_暂定方案.md) - GPIO 映射表
- [采购清单](hardware/采购清单.md) - 器件清单和链接

### 固件文档 (`firmware/`)
- [Firmware README](firmware/README.md) - 固件开发入口
- [CubeIDE 配置指南](firmware/STM32F103ZET6_CubeIDE配置指南.md) - M1-M6 配置索引
- [M1 板级冒烟](firmware/milestones/M1_板级冒烟/) - M1 阶段配置和测试文档

## 目录说明

- `config/`：采样、特征、告警和低功耗参数样例。
- `docs/`：需求、架构、物料、路线图和验收口径。
- `firmware/`：确认硬件基线后放置 STM32CubeIDE/FreeRTOS 工程。
- `hardware/`：硬件基线、接线、模拟前端和功耗测量记录。
- `tests/`：离线样本、算法对照、故障注入和真机测试记录。
- `tools/`：PC 侧数据分析、配置检查和报告辅助工具。

当前接线见 [STM32F103ZET6 引脚分配](hardware/引脚分配_暂定方案.md)；代码按该基线使用 PB8/PB9、PG11、PA0、PE2、PE6、SPI2 和 USART1。实物板卡修订、跳线、电气连接和供电路径仍需在硬件验证时复核。

## 首版代码闭环

`RTC 唤醒 -> 自检 -> 固定窗口采集 -> 特征提取 -> 连续超限判定 -> 串口上报 -> Stop`

代码层面已实现上述闭环；只有在真实硬件上完成对应测试并保留证据后，才能进一步宣称驱动时序、低功耗电流、算法精度或长期稳定性已验证。
