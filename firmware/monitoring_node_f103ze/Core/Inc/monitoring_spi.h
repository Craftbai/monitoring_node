#ifndef MONITORING_SPI_H
#define MONITORING_SPI_H

#include "monitoring_config.h"
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t transfers;
  uint32_t errors;
  uint32_t recoveries;
  uint32_t recovery_failures;
  uint8_t enabled;
} monitoring_spi_status_t;

/* SPI 外设由 CubeMX 配置；配置关闭时，接口明确返回 HAL_ERROR。 */
void MonitoringSpi_Init(void);
HAL_StatusTypeDef MonitoringSpi_Recover(void);
HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx,
                                         uint8_t *rx,
                                         uint16_t length,
                                         uint32_t timeout_ms);
void MonitoringSpi_GetStatus(monitoring_spi_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_SPI_H */
