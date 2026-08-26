#ifndef MONITORING_CONFIG_H
#define MONITORING_CONFIG_H

/*
 * 应用配置集中放在这里。该文件不由 CubeMX 生成，重新生成外设代码时不会
 * 覆盖。通道是否有真实硬件由部署条件决定，固件按正式采集链路运行。
 */

#define MONITOR_SPI_ENABLED              1U // 是否启用 SPI 总线驱动（NRF24/传感器共享总线）
#define MONITOR_NRF24_REPORT_ENABLED     1U // 是否启用 NRF24L01 上报链路；关闭后仅保留 UART 日志
#define MONITOR_NRF24_CHANNEL            76U // 无线通信通道号，范围 0~125
#define MONITOR_NRF24_PAYLOAD_SIZE       32U // 单帧载荷长度，固定为 32 字节，便于接收端按固定帧长解析
#define MONITOR_NRF24_SEND_TIMEOUT_MS    100U // 单次 NRF24 发送等待超时时间，超过后记错误并继续下一周期

#define MONITOR_HW_IWDG_ENABLED          0U // STM32F1 Stop 期间 IWDG 仍可能继续计时；当前只使用软件看门狗

#define MONITOR_CURRENT_SENSOR_ENABLED   1U // 是否启用电流采样链路
#define MONITOR_CURRENT_ZERO_RAW         2068U // ADC 零点基准值，表示无电流时的原始 ADC 读数
#define MONITOR_CURRENT_COUNTS_PER_AMP   83U // 每安培对应的 ADC 计数增量，用于换算 mA

#define MONITOR_TEMP_LIMIT_CENTI         6500L // 温度上限阈值，单位 0.01°C；例 6500 = 65.00°C
#define MONITOR_TEMP_RECOVER_CENTI       6000L // 温度恢复阈值，低于该值后可从告警恢复
#define MONITOR_CURRENT_LIMIT_MA         1200L // 电流上限阈值，单位 mA
#define MONITOR_CURRENT_RECOVER_MA       1000L // 电流恢复阈值，低于该值后视为恢复
#define MONITOR_VIBRATION_LIMIT_MG       350L // 振动上限阈值，单位 mg
#define MONITOR_VIBRATION_RECOVER_MG     300L // 振动恢复阈值，低于该值后视为恢复

#define MONITOR_ALERT_CONFIRM_CYCLES     3U // 连续多少个有效周期超过阈值后才进入 ACTIVE
#define MONITOR_ALERT_RECOVER_CYCLES     3U // 连续多少个有效周期低于恢复阈值后才清除告警

#endif /* MONITORING_CONFIG_H */
