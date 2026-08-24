#ifndef MONITORING_CONFIG_H
#define MONITORING_CONFIG_H

/*
 * 应用配置集中放在这里。该文件不由 CubeMX 生成，重新生成外设代码时不会
 * 覆盖。通道是否有真实硬件由部署条件决定，固件按正式采集链路运行。
 */

/* SPI2/NRF24L01 驱动已完整接入；无线结果上报仍由独立开关控制。 */
#define MONITOR_SPI_ENABLED              1U
#define MONITOR_NRF24_REPORT_ENABLED     1U
#define MONITOR_NRF24_CHANNEL            76U
#define MONITOR_NRF24_PAYLOAD_SIZE       32U
#define MONITOR_NRF24_SEND_TIMEOUT_MS    100U

/* STM32F1 Stop 期间 IWDG 仍可能继续计时，第一版只使用软件看门狗。 */
#define MONITOR_HW_IWDG_ENABLED          0U

/* ACS712 ADC-DMA 链路正式开启；零点和比例保留为可替换标定参数。 */
#define MONITOR_CURRENT_SENSOR_ENABLED   1U
#define MONITOR_CURRENT_ZERO_RAW         2068U
#define MONITOR_CURRENT_COUNTS_PER_AMP   83U

/* 告警阈值采用整数单位，避免在 F103 上引入浮点配置解析。 */
#define MONITOR_TEMP_LIMIT_CENTI         6500L
#define MONITOR_TEMP_RECOVER_CENTI       6000L
#define MONITOR_CURRENT_LIMIT_MA         1200L
#define MONITOR_CURRENT_RECOVER_MA       1000L
#define MONITOR_VIBRATION_LIMIT_MG       350L
#define MONITOR_VIBRATION_RECOVER_MG     300L

/* 连续有效周期确认策略。无效周期不推进确认和恢复计数。 */
#define MONITOR_ALERT_CONFIRM_CYCLES     3U
#define MONITOR_ALERT_RECOVER_CYCLES     3U

#endif /* MONITORING_CONFIG_H */
