/* =============================================================================
 * monitoring_drivers.c - 传感器和模块统一初始化实现
 * ============================================================================= */

#include "monitoring_drivers.h"
#include "monitoring_ds18b20.h"
#include "monitoring_mpu6050.h"
#include "adc.h"
#include "monitoring_nrf24.h"
#include "monitoring_hw_watchdog.h"
#include "monitoring_bus.h"
#include "monitoring_tasks.h"

/* ===== 静态变量 ===== */
static monitoring_drivers_status_t g_drivers_status;

/* ===== 公开 API 实现 ===== */

/**
 * @brief  初始化所有传感器和模块
 */
void MonitoringDrivers_Init(void) {
  /* ===== 传感器初始化（按通道顺序：温度/振动/电流） ===== */

  /* 温度传感器：DS18B20（1-Wire） */
  g_drivers_status.ds18b20_ready = (DS18B20_Init() == MONITORING_OK) ? 1U : 0U;

  /* 振动传感器：MPU6050（I2C1） */
  g_drivers_status.mpu6050_ready = (MPU6050_Init() == MONITORING_OK) ? 1U : 0U;

  /* 电流传感器：ACS712（ADC1 + TIM3 + DMA） */
  g_drivers_status.adc_ready = (HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK) ? 1U : 0U;

  /* ===== 无线模块初始化 ===== */
  monitoring_nrf24_bus_t nrf24_bus = {
    .transfer = MonitoringSpi_Transfer,
    .chip_select = MonitoringTasks_Nrf24ChipSelect,
    .chip_enable = MonitoringTasks_Nrf24ChipEnable,
    .ready = 1U
  };

  MonitoringNrf24_Bind(&nrf24_bus);
  g_drivers_status.nrf24_ready = (MonitoringNrf24_Init() == MONITOR_NRF24_OK) ? 1U : 0U;

  /* ===== 硬件看门狗初始化 ===== */
  MonitoringHardwareWatchdog_Init();
  g_drivers_status.iwdg_ready = MonitoringHardwareWatchdog_IsEnabled();
}

/**
 * @brief  获取驱动就绪状态
 */
void MonitoringDrivers_GetStatus(monitoring_drivers_status_t *status) {
  if (status != NULL) {
    *status = g_drivers_status;
  }
}
