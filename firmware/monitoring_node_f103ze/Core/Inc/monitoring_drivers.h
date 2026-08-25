/* =============================================================================
 * monitoring_drivers.h - 传感器和模块统一初始化接口
 *
 * 职责：统一管理所有传感器和模块的初始化，提供就绪状态查询。
 *
 * 设计原则：
 *   - 所有传感器在此处显式初始化
 *   - 按通道顺序：温度 → 振动 → 电流 → 无线 → 看门狗
 *   - 传感器失败不影响系统启动（通道降级）
 * ============================================================================= */

#ifndef MONITORING_DRIVERS_H
#define MONITORING_DRIVERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  驱动类型枚举（用于状态查询）
 */
typedef enum {
  DRIVER_DS18B20,       /* 温度传感器 */
  DRIVER_MPU6050,       /* 振动传感器 */
  DRIVER_ADC,           /* 电流传感器（ADC） */
  DRIVER_NRF24,         /* 无线模块 */
  DRIVER_IWDG           /* 硬件看门狗 */
} monitoring_driver_type_t;

/**
 * @brief  驱动就绪状态
 */
typedef struct {
  uint8_t ds18b20_ready;    /* 温度传感器就绪 */
  uint8_t mpu6050_ready;    /* 振动传感器就绪 */
  uint8_t adc_ready;        /* 电流传感器（ADC）就绪 */
  uint8_t nrf24_ready;      /* 无线模块就绪 */
  uint8_t iwdg_ready;       /* 硬件看门狗就绪 */
} monitoring_drivers_status_t;

/**
 * @brief  初始化所有传感器和模块
 * @note   按依赖顺序初始化：传感器 → 无线 → 看门狗
 * @note   传感器失败不影响系统启动（通道降级运行）
 */
void MonitoringDrivers_Init(void);

/**
 * @brief  Stop 模式唤醒后恢复所有传感器和模块
 * @note   重新初始化所有外设和传感器
 * @note   统一管理 Stop 唤醒后的恢复逻辑
 * @retval 1: 恢复成功，0: 恢复失败（ADC 校准失败）
 */
uint8_t MonitoringDrivers_Resume(void);

/**
 * @brief  获取驱动就绪状态
 * @param  status: 输出状态结构体
 */
void MonitoringDrivers_GetStatus(monitoring_drivers_status_t *status);

/**
 * @brief  查询指定驱动是否就绪
 * @param  type: 驱动类型
 * @retval 1: 就绪，0: 未就绪
 */
uint8_t MonitoringDrivers_IsReady(monitoring_driver_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_DRIVERS_H */
