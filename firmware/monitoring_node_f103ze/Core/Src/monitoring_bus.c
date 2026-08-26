/* =============================================================================
 * monitoring_bus.c - 通信接口统一封装实现
 * ============================================================================= */

#include "monitoring_bus.h"
#include "monitoring_config.h"

#if MONITOR_SPI_ENABLED
#include "spi.h"
#endif

#include "i2c.h"

/* ===== 静态变量 ===== */
static monitoring_bus_status_t g_spi_status;
static monitoring_bus_status_t g_i2c_status;

/* ===== 公开 API 实现 ===== */

/**
 * @brief  初始化所有通信接口
 */
void MonitoringBus_Init(void) {
  MonitoringSpi_Init();
  MonitoringI2c_Init();
}

/* ===== SPI 实现 ===== */

void MonitoringSpi_Init(void) {
  g_spi_status.transfers = 0U;
  g_spi_status.errors = 0U;
  g_spi_status.recoveries = 0U;
  g_spi_status.recovery_failures = 0U;
  g_spi_status.enabled = MONITOR_SPI_ENABLED;
}

HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx, uint8_t *rx,
                                         uint16_t length, uint32_t timeout_ms) {
  HAL_StatusTypeDef result;

  if (tx == NULL || rx == NULL || length == 0U) {
    g_spi_status.errors++;
    return HAL_ERROR;
  }

#if MONITOR_SPI_ENABLED
  result = HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)tx, rx, length, timeout_ms);
#else
  (void)timeout_ms;
  result = HAL_ERROR;
#endif

  if (result == HAL_OK) {
    g_spi_status.transfers++;
  } else {
    g_spi_status.errors++;
    (void)MonitoringSpi_Recover();
  }
  return result;
}

HAL_StatusTypeDef MonitoringSpi_Recover(void) {
#if MONITOR_SPI_ENABLED
  HAL_StatusTypeDef status;

  status = HAL_SPI_DeInit(&hspi2);
  if (status == HAL_OK) {
    MX_SPI2_Init();
    status = (hspi2.State == HAL_SPI_STATE_READY) ? HAL_OK : HAL_ERROR;
  }

  if (status == HAL_OK) {
    g_spi_status.recoveries++;
  } else {
    g_spi_status.recovery_failures++;
  }
  return status;
#else
  g_spi_status.recovery_failures++;
  return HAL_ERROR;
#endif
}

void MonitoringSpi_GetStatus(monitoring_bus_status_t *status) {
  if (status != NULL) {
    *status = g_spi_status;
  }
}

/* ===== I2C 实现（新增） ===== */

void MonitoringI2c_Init(void) {
  g_i2c_status.transfers = 0U;
  g_i2c_status.errors = 0U;
  g_i2c_status.recoveries = 0U;
  g_i2c_status.recovery_failures = 0U;
  g_i2c_status.enabled = 1U;
}

HAL_StatusTypeDef MonitoringI2c_MemRead(uint16_t dev_addr, uint16_t mem_addr,
                                        uint8_t *data, uint16_t length,
                                        uint32_t timeout_ms) {
  HAL_StatusTypeDef result;

  if (data == NULL || length == 0U) {
    g_i2c_status.errors++;
    return HAL_ERROR;
  }

  result = HAL_I2C_Mem_Read(&hi2c2, dev_addr, mem_addr,
                            I2C_MEMADD_SIZE_8BIT, data, length, timeout_ms);

  if (result == HAL_OK) {
    g_i2c_status.transfers++;
  } else {
    g_i2c_status.errors++;
    (void)MonitoringI2c_Recover();
  }
  return result;
}

HAL_StatusTypeDef MonitoringI2c_MemWrite(uint16_t dev_addr, uint16_t mem_addr,
                                         const uint8_t *data, uint16_t length,
                                         uint32_t timeout_ms) {
  HAL_StatusTypeDef result;

  if (data == NULL || length == 0U) {
    g_i2c_status.errors++;
    return HAL_ERROR;
  }

  result = HAL_I2C_Mem_Write(&hi2c2, dev_addr, mem_addr,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)data,
                             length, timeout_ms);

  if (result == HAL_OK) {
    g_i2c_status.transfers++;
  } else {
    g_i2c_status.errors++;
    (void)MonitoringI2c_Recover();
  }
  return result;
}

HAL_StatusTypeDef MonitoringI2c_Recover(void) {
  HAL_StatusTypeDef status;

  status = HAL_I2C_DeInit(&hi2c2);
  if (status == HAL_OK) {
    MX_I2C2_Init();
    status = (hi2c2.State == HAL_I2C_STATE_READY) ? HAL_OK : HAL_ERROR;
  }

  if (status == HAL_OK) {
    g_i2c_status.recoveries++;
  } else {
    g_i2c_status.recovery_failures++;
  }
  return status;
}

void MonitoringI2c_GetStatus(monitoring_bus_status_t *status) {
  if (status != NULL) {
    *status = g_i2c_status;
  }
}
