#ifndef MONITORING_NRF24_H
#define MONITORING_NRF24_H

#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  MONITOR_NRF24_OK = 0,
  MONITOR_NRF24_ARGUMENT,
  MONITOR_NRF24_NOT_READY,
  MONITOR_NRF24_BUS_ERROR,
  MONITOR_NRF24_NOT_PRESENT,
  MONITOR_NRF24_TIMEOUT,
  MONITOR_NRF24_MAX_RETRY
} monitoring_nrf24_status_t;

typedef HAL_StatusTypeDef (*monitoring_nrf24_transfer_fn)(
  const uint8_t *tx, uint8_t *rx, uint16_t length, uint32_t timeout_ms);
typedef void (*monitoring_nrf24_chip_select_fn)(uint8_t active);
typedef void (*monitoring_nrf24_chip_enable_fn)(uint8_t active);

typedef struct
{
  monitoring_nrf24_transfer_fn transfer;
  monitoring_nrf24_chip_select_fn chip_select;
  monitoring_nrf24_chip_enable_fn chip_enable;
  uint8_t ready;
} monitoring_nrf24_bus_t;

/* NRF24L01 的 SPI、CSN、CE 均通过回调绑定，驱动本身不直接依赖 GPIO。 */
void MonitoringNrf24_Bind(const monitoring_nrf24_bus_t *bus);
monitoring_nrf24_status_t MonitoringNrf24_Init(void);
uint8_t MonitoringNrf24_IsReady(void);
monitoring_nrf24_status_t MonitoringNrf24_SendPayload(const uint8_t *data,
                                                      uint8_t length);
monitoring_nrf24_status_t MonitoringNrf24_StartListening(void);
monitoring_nrf24_status_t MonitoringNrf24_StopListening(void);
uint8_t MonitoringNrf24_Available(void);
monitoring_nrf24_status_t MonitoringNrf24_SetTxAddress(const uint8_t address[5]);
monitoring_nrf24_status_t MonitoringNrf24_ClearIrq(void);
monitoring_nrf24_status_t MonitoringNrf24_ReadStatus(uint8_t *status);
void MonitoringNrf24_Sleep(void);
void MonitoringNrf24_SetCe(uint8_t active);
void MonitoringNrf24_NotifyIrqFromISR(void);
uint8_t MonitoringNrf24_TakeIrq(void);
monitoring_nrf24_status_t MonitoringNrf24_ReadRegister(uint8_t reg,
                                                        uint8_t *value);
monitoring_nrf24_status_t MonitoringNrf24_WriteRegister(uint8_t reg,
                                                         uint8_t value);
monitoring_nrf24_status_t MonitoringNrf24_ReadPayload(uint8_t *data,
                                                       uint8_t length);
monitoring_nrf24_status_t MonitoringNrf24_WritePayload(const uint8_t *data,
                                                        uint8_t length);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_NRF24_H */
