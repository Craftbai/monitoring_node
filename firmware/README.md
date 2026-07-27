# 固件目录规划

建议使用 STM32CubeIDE 建立工程后，按以下方式组织自编代码：

- `Core/App/`：任务创建、状态机和应用入口。
- `Core/Drivers/`：传感器、ADC DMA、串口与总线恢复。
- `Core/Algorithm/`：滤波、窗函数、FFT、特征和告警。
- `Core/Config/`：阈值和采样参数。
- `Core/Diagnostics/`：任务心跳、错误统计和日志。

CubeMX 自动生成的 HAL 文件保持可再生成，自编业务代码放在受控目录或官方用户代码区。
