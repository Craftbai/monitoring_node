# 固件说明

当前工程为 STM32CubeIDE 生成的 STM32F103ZET6/LQFP144 固件工程，位于
`firmware/monitoring_node_f103ze/`。工程已经纳入 FreeRTOS、三类采集、基础 DSP、告警、RTC/Stop、USART1 和 SPI2/NRF24L01 代码。

正式配置为 `300 秒` RTC 周期；测试周期仍保留为 `5 秒` 编译选项。当前代码版本已提交到仓库 `main` 分支。本文档描述代码结构，不替代真实传感器和功耗测试记录。

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

## 当前工程检查

1. 确认 `hardware/硬件基线.md` 已存在，CubeMX 目标为 STM32F103ZET6/LQFP144，且版本与工程一致。
2. 当前已开启 USART1、RTC、Stop、I2C1、SPI2、ADC1 + DMA 和 FreeRTOS。
3. SPI2 使用 PB13/PB14/PB15，NRF24L01 使用 PB12/PE3/PE4 作为 CSN/CE/IRQ。
4. 采样块、DMA/FIFO 缓冲定义了所有权、长度、序号和溢出处理。
5. `config/thresholds.example.json` 只作为配置参考，MCU 使用 `monitoring_config.h` 中的静态配置常量。

## FreeRTOS 任务与数据链路

当前 STM32CubeIDE 工程已经接入完整任务链路：

- `defaultTask`：FreeRTOS 基础 LED 心跳任务；`cycle_task`：等待 RTC Alarm 事件并生成一个 `cycle_id`；
- `acquisition_task`：协调 DS18B20、MPU6050 和 ADC-DMA，使用固定采样块池；无传感器时按无效通道降级，不填充伪造数据；
- `processing_task`：执行去直流、窗函数、RMS、峰峰值、峰值因子、频带能量、电流和温度特征计算；
- `report_task`：通过 USART1 输出结构化结果，并按开关尝试 NRF24L01 无线上报；
- `health_task`：统计任务心跳、周期计数、队列水位、丢弃计数和错误状态；
- `watchdog_task`：检查关键任务是否持续推进，第一版使用软件看门狗。

任务间使用 CMSIS-RTOS V2 消息队列、事件和固定采样块池，数据链路为“RTC Alarm -> 周期协调 -> 统一采集 -> DSP 处理 -> 告警 -> UART/NRF24L01 上报 -> Stop”。无传感器时仍输出无效标志；这表示降级运行，不表示零值测量有效。

## 当前代码边界

- 已完成：代码模块、任务划分、接口、错误标志、告警状态机和 CubeIDE 工程接入。
- 尚未由代码单独证明：真实传感器接线与时序、NRF24L01 对端通信、ACS712 标定精度、Stop 实际电流和长时间硬件稳定性。
- 硬件 IWDG 当前关闭：STM32F1 在长 Stop 周期中可能继续计时，代码使用软件看门狗监测。

主要应用文件：

- `monitoring_node_f103ze/Core/Inc/monitoring_tasks.h`
- `monitoring_node_f103ze/Core/Src/monitoring_tasks.c`
- `monitoring_node_f103ze/Core/Src/freertos.c`
