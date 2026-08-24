/* =============================================================================
 * monitoring_nrf24.h - NRF24L01 无线模块驱动接口
 *
 * 职责：封装 NRF24L01 的寄存器操作，提供初始化、发送、接收、休眠接口，
 *       配合 monitoring_spi 完成无线结果上报。
 *
 * 当前配置：
 *   - 频道：76（2.476GHz）
 *   - 地址宽度：5 字节
 *   - 载荷长度：32 字节（固定长度，RX_PW_P0 = 32）
 *   - 速率：1Mbps
 *   - 发射功率：0dBm
 *   - 自动重传：15 次，延时 750us
 *   - CRC：2 字节
 *
 * 设计原则：
 *   - 驱动不直接依赖 GPIO，通过回调函数操作 CSN/CE
 *   - 传输函数由外部提供（MonitoringSpi_Transfer）
 *   - 模块缺失时返回 NOT_PRESENT，不阻止主链路运行
 * ============================================================================= */

#ifndef MONITORING_NRF24_H
#define MONITORING_NRF24_H

#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  NRF24 状态码枚举
 */
typedef enum
{
  MONITOR_NRF24_OK = 0,         /* 操作成功 */
  MONITOR_NRF24_ARGUMENT,       /* 参数错误（NULL 指针、长度非法） */
  MONITOR_NRF24_NOT_READY,      /* 总线未就绪（SPI 未启用或未绑定） */
  MONITOR_NRF24_BUS_ERROR,      /* SPI 传输错误 */
  MONITOR_NRF24_NOT_PRESENT,    /* 模块不在线（配置寄存器读回值不匹配） */
  MONITOR_NRF24_TIMEOUT,        /* 发送超时（等待 TX_DS/MAX_RT 标志） */
  MONITOR_NRF24_MAX_RETRY       /* 达到最大重传次数（MAX_RT 标志置位） */
} monitoring_nrf24_status_t;

/* 回调函数类型定义 */
typedef HAL_StatusTypeDef (*monitoring_nrf24_transfer_fn)(
  const uint8_t *tx, uint8_t *rx, uint16_t length, uint32_t timeout_ms);
typedef void (*monitoring_nrf24_chip_select_fn)(uint8_t active);
typedef void (*monitoring_nrf24_chip_enable_fn)(uint8_t active);

/**
 * @brief  NRF24 总线接口结构体
 */
typedef struct
{
  monitoring_nrf24_transfer_fn transfer;       /* SPI 传输函数 */
  monitoring_nrf24_chip_select_fn chip_select; /* CSN 控制回调 */
  monitoring_nrf24_chip_enable_fn chip_enable; /* CE 控制回调 */
  uint8_t ready;                               /* 总线就绪标志 */
} monitoring_nrf24_bus_t;

/**
 * @brief  绑定总线接口（SPI 传输、CSN、CE）
 * @param  bus: 总线接口结构体指针
 * @note   在 MonitoringTasks_Create() 中调用，绑定 MonitoringSpi_Transfer 和 GPIO 回调
 */
void MonitoringNrf24_Bind(const monitoring_nrf24_bus_t *bus);

/**
 * @brief  初始化 NRF24L01（配置寄存器、设置地址、清除标志）
 * @return 状态码（OK/NOT_PRESENT/BUS_ERROR）
 * @note   初始化流程：读回 CONFIG 寄存器验证通信 → 配置模式/频道/速率 → 设置地址
 */
monitoring_nrf24_status_t MonitoringNrf24_Init(void);

/**
 * @brief  查询 NRF24 是否在线且就绪
 * @return 1=就绪，0=未就绪
 */
uint8_t MonitoringNrf24_IsReady(void);

/**
 * @brief  发送载荷（TX 模式，阻塞等待发送完成）
 * @param  data: 载荷数据指针
 * @param  length: 载荷长度（最大 32 字节）
 * @return 状态码（OK/TIMEOUT/MAX_RETRY）
 * @note   发送流程：写载荷 → CE 高电平 10us → 等待 TX_DS/MAX_RT 标志（100ms 超时）
 */
monitoring_nrf24_status_t MonitoringNrf24_SendPayload(const uint8_t *data,
                                                      uint8_t length);

/* 其他接口省略详细注释（较少使用） */
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
