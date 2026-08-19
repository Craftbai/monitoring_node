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
