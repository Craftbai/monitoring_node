#include "monitoring_spi.h"

#if MONITOR_SPI_ENABLED
#include "spi.h"
#endif

static monitoring_spi_status_t g_spi_status;

void MonitoringSpi_Init(void)
{
  g_spi_status.transfers = 0U;
  g_spi_status.errors = 0U;
  g_spi_status.recoveries = 0U;
  g_spi_status.recovery_failures = 0U;
  g_spi_status.enabled = MONITOR_SPI_ENABLED != 0U ? 1U : 0U;
}

HAL_StatusTypeDef MonitoringSpi_Recover(void)
{
#if MONITOR_SPI_ENABLED
  HAL_StatusTypeDef status;

  /* SPI 轮询超时后释放并重新初始化外设，恢复 CR1/CR2 和 GPIO 状态。 */
  status = HAL_SPI_DeInit(&hspi2);
  if (status == HAL_OK)
  {
    MX_SPI2_Init();
    status = (hspi2.State == HAL_SPI_STATE_READY) ? HAL_OK : HAL_ERROR;
  }
  if (status == HAL_OK)
  {
    g_spi_status.recoveries++;
  }
  else
  {
    g_spi_status.recovery_failures++;
  }
  return status;
#else
  g_spi_status.recovery_failures++;
  return HAL_ERROR;
#endif
}

HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx,
                                         uint8_t *rx,
                                         uint16_t length,
                                         uint32_t timeout_ms)
{
  HAL_StatusTypeDef result;

  if (tx == NULL || rx == NULL || length == 0U)
  {
    g_spi_status.errors++;
    return HAL_ERROR;
  }

#if MONITOR_SPI_ENABLED
  result = HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)tx, rx,
                                   length, timeout_ms);
#else
  (void)timeout_ms;
  result = HAL_ERROR;
#endif

  if (result == HAL_OK)
  {
    g_spi_status.transfers++;
  }
  else
  {
    g_spi_status.errors++;
    (void)MonitoringSpi_Recover();
  }
  return result;
}

void MonitoringSpi_GetStatus(monitoring_spi_status_t *status)
{
  if (status != NULL)
  {
    *status = g_spi_status;
  }
}
