/* =============================================================================
 * monitoring_bus.h - 通信接口统一封装（SPI + I2C）
 *
 * 职责：为上层模块提供统一的通信接口，封装错误恢复、统计、超时处理。
 *
 * 设计原则：
 *   - SPI 和 I2C 完全对称设计
 *   - 统一的错误恢复策略（DeInit → Init）
 *   - 统一的统计接口（transfers/errors/recoveries）
 *   - 自动错误恢复（传输失败时自动尝试恢复）
 * ============================================================================= */

#ifndef MONITORING_BUS_H
#define MONITORING_BUS_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  通信总线统计信息
 */
typedef struct {
  uint32_t transfers;          /* 成功传输次数 */
  uint32_t errors;             /* 传输错误次数 */
  uint32_t recoveries;         /* 成功恢复次数 */
  uint32_t recovery_failures;  /* 恢复失败次数 */
  uint8_t enabled;             /* 总线是否启用 */
} monitoring_bus_status_t;

/**
 * @brief  初始化所有通信接口（SPI + I2C）
 * @note   在 MonitoringTasks_Create() 中调用
 */
void MonitoringBus_Init(void);

/* ===== SPI 接口 ===== */

/**
 * @brief  初始化 SPI 统计信息
 */
void MonitoringSpi_Init(void);

/**
 * @brief  SPI 全双工传输（阻塞式）
 * @param  tx: 发送缓冲区
 * @param  rx: 接收缓冲区
 * @param  length: 传输字节数
 * @param  timeout_ms: 超时时间（毫秒）
 * @return HAL_OK=成功，HAL_ERROR=失败
 * @note   传输失败时自动调用 MonitoringSpi_Recover()
 */
HAL_StatusTypeDef MonitoringSpi_Transfer(const uint8_t *tx, uint8_t *rx,
                                         uint16_t length, uint32_t timeout_ms);

/**
 * @brief  SPI 错误恢复（DeInit → Init）
 * @return HAL_OK=恢复成功，HAL_ERROR=恢复失败
 */
HAL_StatusTypeDef MonitoringSpi_Recover(void);

/**
 * @brief  获取 SPI 统计信息
 * @param  status: 输出结构体指针
 */
void MonitoringSpi_GetStatus(monitoring_bus_status_t *status);

/* ===== I2C 接口（新增，与 SPI 对称设计） ===== */

/**
 * @brief  初始化 I2C 统计信息
 */
void MonitoringI2c_Init(void);

/**
 * @brief  I2C 内存读取（阻塞式）
 * @param  dev_addr: 设备地址（7 位地址左移 1 位）
 * @param  mem_addr: 内存地址（寄存器地址）
 * @param  data: 接收缓冲区
 * @param  length: 读取字节数
 * @param  timeout_ms: 超时时间（毫秒）
 * @return HAL_OK=成功，HAL_ERROR=失败
 * @note   读取失败时自动调用 MonitoringI2c_Recover()
 */
HAL_StatusTypeDef MonitoringI2c_MemRead(uint16_t dev_addr, uint16_t mem_addr,
                                        uint8_t *data, uint16_t length,
                                        uint32_t timeout_ms);

/**
 * @brief  I2C 内存写入（阻塞式）
 * @param  dev_addr: 设备地址（7 位地址左移 1 位）
 * @param  mem_addr: 内存地址（寄存器地址）
 * @param  data: 发送缓冲区
 * @param  length: 写入字节数
 * @param  timeout_ms: 超时时间（毫秒）
 * @return HAL_OK=成功，HAL_ERROR=失败
 * @note   写入失败时自动调用 MonitoringI2c_Recover()
 */
HAL_StatusTypeDef MonitoringI2c_MemWrite(uint16_t dev_addr, uint16_t mem_addr,
                                         const uint8_t *data, uint16_t length,
                                         uint32_t timeout_ms);

/**
 * @brief  I2C 错误恢复（DeInit → Init）
 * @return HAL_OK=恢复成功，HAL_ERROR=恢复失败
 */
HAL_StatusTypeDef MonitoringI2c_Recover(void);

/**
 * @brief  获取 I2C 统计信息
 * @param  status: 输出结构体指针
 */
void MonitoringI2c_GetStatus(monitoring_bus_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_BUS_H */
