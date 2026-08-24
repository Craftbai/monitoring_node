/* =============================================================================
 * monitoring_spi.c - SPI2 传输封装与错误恢复实现
 *
 * 为什么需要这个封装层？
 *   NRF24L01 驱动需要频繁进行短字节传输（1-32 字节），传输失败时
 *   需要明确的恢复策略。直接使用 HAL_SPI_TransmitReceive 无法统一
 *   处理错误恢复和统计，因此封装一个传输层：
 *   - 统一的错误处理（自动恢复）
 *   - 统一的统计（成功/失败/恢复）
 *   - 统一的配置开关（MONITOR_SPI_ENABLED）
 *
 * 错误恢复原理：
 *   SPI 轮询超时可能导致外设状态机卡住（BUSY 标志不清除），此时
 *   需要 DeInit → Init 重置硬件状态。HAL 库的 DeInit 会清除 CR1
 *   使能位，Init 会重新配置 CR1/CR2 和 GPIO。
 * ============================================================================= */

#include "monitoring_spi.h"

#if MONITOR_SPI_ENABLED
#include "spi.h"
#endif

/* ===== 静态变量 ===== */
/* SPI 统计信息（由 health_task 定期读取） */
static monitoring_spi_status_t g_spi_status;

/* ===== 公开 API 实现 ===== */

/**
 * @brief  初始化 SPI 统计信息
 * @note   在 MonitoringTasks_Create() 中调用
 */
void MonitoringSpi_Init(void)
{
  g_spi_status.transfers = 0U;
  g_spi_status.errors = 0U;
  g_spi_status.recoveries = 0U;
  g_spi_status.recovery_failures = 0U;
  g_spi_status.enabled = MONITOR_SPI_ENABLED != 0U ? 1U : 0U;
}

/**
 * @brief  SPI 错误恢复（DeInit → Init）
 * @return HAL_OK=恢复成功，HAL_ERROR=恢复失败
 *
 * @note   恢复流程：
 *         1. HAL_SPI_DeInit(&hspi2) - 清除 CR1 使能位，释放硬件
 *         2. MX_SPI2_Init() - 重新配置 CR1/CR2/GPIO/DMA（若使能）
 *         3. 检查 hspi2.State == HAL_SPI_STATE_READY
 *
 * @note   统计更新：
 *         - 成功：recoveries++
 *         - 失败：recovery_failures++
 */
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
  /* SPI 未启用时，恢复操作无意义，记录为恢复失败 */
  g_spi_status.recovery_failures++;
  return HAL_ERROR;
#endif
}

/**
 * @brief  SPI 全双工传输（阻塞式）
 * @param  tx: 发送缓冲区（不能为 NULL）
 * @param  rx: 接收缓冲区（不能为 NULL）
 * @param  length: 传输字节数（不能为 0）
 * @param  timeout_ms: 超时时间（毫秒）
 * @return HAL_OK=成功，HAL_ERROR=失败或参数错误
 *
 * @note   参数检查：
 *         tx/rx 为 NULL 或 length 为 0 时，直接返回 HAL_ERROR
 *
 * @note   错误处理：
 *         传输失败时自动调用 MonitoringSpi_Recover()，尝试恢复外设。
 *         即使恢复失败，也返回原始的传输错误（不传播恢复错误）。
 *
 * @note   调用上下文：
 *         由 NRF24 驱动在任务上下文中调用（不在中断中使用）
 */
HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx,
                                         uint8_t *rx,
                                         uint16_t length,
                                         uint32_t timeout_ms)
{
  HAL_StatusTypeDef result;

  /* 参数检查：NULL 指针或长度为 0 */
  if (tx == NULL || rx == NULL || length == 0U)
  {
    g_spi_status.errors++;
    return HAL_ERROR;
  }

#if MONITOR_SPI_ENABLED
  /* 调用 HAL 库的全双工传输函数（阻塞式）
   * 注：HAL_SPI_TransmitReceive 的 tx 参数声明为 uint8_t*（非 const），
   * 但实际不会修改发送缓冲区，这里强制转换以适配接口 */
  result = HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)tx, rx,
                                   length, timeout_ms);
#else
  /* SPI 未启用时，直接返回错误 */
  (void)timeout_ms;
  result = HAL_ERROR;
#endif

  /* 统计更新与错误恢复 */
  if (result == HAL_OK)
  {
    g_spi_status.transfers++;
  }
  else
  {
    g_spi_status.errors++;
    /* 传输失败时尝试恢复，忽略恢复结果（调用者根据传输结果决策） */
    (void)MonitoringSpi_Recover();
  }
  return result;
}

/**
 * @brief  获取 SPI 统计信息
 * @param  status: 输出结构体指针
 * @note   由 health_task 定期调用，输出到 UART 日志
 */
void MonitoringSpi_GetStatus(monitoring_spi_status_t *status)
{
  if (status != NULL)
  {
    *status = g_spi_status;
  }
}
