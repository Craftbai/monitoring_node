/* =============================================================================
 * monitoring_spi.h - SPI2 传输封装与错误恢复接口
 *
 * 职责：为 NRF24L01 驱动提供 SPI 传输接口，统计传输次数和错误计数，
 *       并在传输失败时自动尝试恢复（DeInit → Init）。
 *
 * 当前配置：
 *   - SPI2（PB12=NSS, PB13=SCK, PB14=MISO, PB15=MOSI）
 *   - 模式：主机，全双工
 *   - 速率：36MHz / 32 = 1.125MHz（NRF24L01 最大 10MHz）
 *   - 极性：CPOL=0, CPHA=0（模式 0）
 *   - 数据位：8 位，MSB 先行
 *
 * 错误恢复策略：
 *   传输超时或错误时，自动调用 MonitoringSpi_Recover()，尝试释放并
 *   重新初始化外设，恢复 CR1/CR2 和 GPIO 状态。如果恢复失败，调用者
 *   需要降级处理（例如 NRF24 驱动返回 NOT_READY）。
 * ============================================================================= */

#ifndef MONITORING_SPI_H
#define MONITORING_SPI_H

#include "monitoring_config.h"
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  SPI 统计信息结构体
 */
typedef struct
{
  uint32_t transfers;          /* 成功传输次数 */
  uint32_t errors;             /* 传输错误次数（超时/总线错误） */
  uint32_t recoveries;         /* 成功恢复次数 */
  uint32_t recovery_failures;  /* 恢复失败次数 */
  uint8_t enabled;             /* SPI 是否启用（MONITOR_SPI_ENABLED） */
} monitoring_spi_status_t;

/**
 * @brief  初始化 SPI 统计信息
 * @note   在 MonitoringTasks_Create() 中调用
 * @note   清零所有计数器，设置 enabled 标志
 */
void MonitoringSpi_Init(void);

/**
 * @brief  SPI 错误恢复（DeInit → Init）
 * @return HAL_OK=恢复成功，HAL_ERROR=恢复失败
 * @note   恢复流程：
 *         1. HAL_SPI_DeInit(&hspi2)
 *         2. MX_SPI2_Init()（重新配置 CR1/CR2/GPIO）
 *         3. 检查 hspi2.State == HAL_SPI_STATE_READY
 * @note   恢复成功时 recoveries++，失败时 recovery_failures++
 */
HAL_StatusTypeDef MonitoringSpi_Recover(void);

/**
 * @brief  SPI 全双工传输（阻塞式）
 * @param  tx: 发送缓冲区（不能为 NULL）
 * @param  rx: 接收缓冲区（不能为 NULL）
 * @param  length: 传输字节数（不能为 0）
 * @param  timeout_ms: 超时时间（毫秒）
 * @return HAL_OK=成功，HAL_ERROR=失败或参数错误
 *
 * @note   传输流程：
 *         1. 参数检查（NULL/length=0 直接返回 HAL_ERROR）
 *         2. 调用 HAL_SPI_TransmitReceive（阻塞式）
 *         3. 成功时 transfers++
 *         4. 失败时 errors++，并自动调用 MonitoringSpi_Recover()
 *
 * @note   调用者：NRF24 驱动的寄存器读写函数
 */
HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx,
                                         uint8_t *rx,
                                         uint16_t length,
                                         uint32_t timeout_ms);

/**
 * @brief  获取 SPI 统计信息
 * @param  status: 输出结构体指针
 * @note   由 health_task 定期调用，输出到 UART 日志
 */
void MonitoringSpi_GetStatus(monitoring_spi_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_SPI_H */
