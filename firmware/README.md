# 固件目录规划

确认硬件基线后，使用 STM32CubeIDE 建立工程。不要在硬件型号未定时复制一份绑定错误引脚的工程作为“模板”。

当前固件目录没有 `.ioc` 目标工程。在完成文档和引脚复核后，应新建 STM32F103ZET6/LQFP144 工程并按基线配置外设，不从其他封装的 `.ioc` 手工替换 MCU 标识字段。

完整操作步骤见 [STM32F103ZET6 STM32CubeIDE 完整配置教程](STM32F103ZET6_CubeIDE完整配置教程.md)，M1 最小验证范围见 [M1 板级冒烟配置](M1_板级冒烟配置.md)。

## 自编代码目录

- `Core/App/`：周期状态机、任务创建、告警策略和应用入口。
- `Core/Acquisition/`：采样块、固定块池、振动/温度/电流采集服务。
- `Core/Drivers/`：传感器、ADC DMA、RTC、串口与总线恢复适配。
- `Core/Algorithm/`：标定、滤波、窗函数、FFT、特征和数值检查。
- `Core/Platform/`：配置版本、时间、错误码、日志和看门狗。
- `Core/Telemetry/`：周期结果编码、UART 或选定无线链路发送。
- `AppTests/`：不依赖 HAL 的算法、告警状态机和故障注入测试。

## 生成代码边界

CubeMX 自动生成的 HAL、启动文件和中断框架保持可再生成。自编代码放在受控目录或官方用户代码区；不得直接修改生成文件来隐藏业务逻辑。DMA 回调只提交缓冲索引，算法、日志和上报均在任务上下文执行。

## 首次工程检查

1. 确认 `hardware/硬件基线.md` 已存在，CubeMX 目标为 STM32F103ZET6/LQFP144，且版本与工程一致。
2. 先验证 UART、RTC、GPIO 和 Stop，再打开 SPI、I2C、ADC DMA。
3. 为每个 DMA/FIFO 缓冲定义所有权、长度、序号和溢出处理。
4. 启动日志输出固件版本、配置版本、复位原因和硬件基线版本。
5. 不把 `config/thresholds.example.json` 当作 MCU 运行时文件；固件使用经过检查的版本化配置结构。

## 当前 FreeRTOS 任务接入状态（2026-08-23）

当前 STM32CubeIDE 工程已经接入第一版任务链路：

- `defaultTask`：FreeRTOS 基础 LED 心跳任务；`cycle_task`：等待 RTC Alarm 事件并生成一个 `cycle_id`；
- `acquisition_task`：从固定采样块池取得块，提交采集结果；当前无真实传感器和 DMA 驱动时，明确标记三路通道无效，不填充伪造数据；
- `processing_task`：消费采样块，生成带 `valid_mask`、错误位图和处理耗时的周期结果，并归还采样块；
- `report_task`：通过 USART1 输出结构化周期结果；后续可替换为 UART/无线传输适配；
- `health_task`：输出任务链路心跳、周期计数、队列深度和丢弃计数。

任务间使用 CMSIS-RTOS V2 消息队列和固定采样块池，当前实现的目标是先证明“RTC Alarm -> 周期协调 -> 采集 -> 处理 -> UART 上报”的所有权和数据流转。真实 SPI 振动采集、温度驱动、ADC DMA、电流标定、CMSIS-DSP 和 Stop 门禁仍需逐项接入，不能把当前无传感器的无效结果写成真实测量能力。

主要应用文件：

- `monitoring_node_f103ze/Core/Inc/monitoring_tasks.h`
- `monitoring_node_f103ze/Core/Src/monitoring_tasks.c`
- `monitoring_node_f103ze/Core/Src/freertos.c`