/* =============================================================================
 * monitoring_status.h - 统一状态码定义
 *
 * 职责：为所有模块提供统一的返回值类型，避免每个模块定义自己的错误码。
 *
 * 设计原则：
 *   - 所有模块使用同一套错误码
 *   - 错误码值从 0 开始递增
 *   - OK 固定为 0（与 HAL_OK 一致）
 * ============================================================================= */

#ifndef MONITORING_STATUS_H
#define MONITORING_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  统一状态码枚举
 */
typedef enum {
  MONITORING_OK = 0,              /* 操作成功 */
  MONITORING_ERROR,               /* 通用错误 */
  MONITORING_ERROR_TIMEOUT,       /* 超时错误 */
  MONITORING_ERROR_NOT_PRESENT,   /* 设备不在线 */
  MONITORING_ERROR_CRC,           /* CRC 校验错误 */
  MONITORING_ERROR_ARGUMENT,      /* 参数错误（NULL 指针、非法值） */
  MONITORING_ERROR_NOT_READY,     /* 未就绪（总线/设备未初始化） */
  MONITORING_ERROR_BUS,           /* 总线错误（SPI/I2C 传输失败） */
  MONITORING_ERROR_MAX_RETRY      /* 达到最大重试次数 */
} monitoring_status_t;

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_STATUS_H */
