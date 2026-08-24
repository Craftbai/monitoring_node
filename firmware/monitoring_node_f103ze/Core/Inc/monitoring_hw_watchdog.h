/* =============================================================================
 * monitoring_hw_watchdog.h - 硬件独立看门狗（IWDG）控制接口
 *
 * 职责：提供 IWDG 初始化、喂狗和状态查询接口，配合软件看门狗共同
 *       监测系统健康状态。
 *
 * 当前状态：首版禁用硬件看门狗（MONITOR_HW_IWDG_ENABLED = 0）
 *   原因：STM32F1 在 Stop 模式下 IWDG 可能继续计时，需要额外的低功耗
 *         喂狗策略。当前仅使用软件看门狗（watchdog_task）监测任务心跳。
 *
 * 启用条件（未来版本）：
 *   1. 验证 Stop 模式下 IWDG 的实际行为（LSI 是否停振）
 *   2. 确定进 Stop 前的喂狗时序（最后一次喂狗 → RTC 闹钟设置 → 进 Stop）
 *   3. 确保 RTC 周期 < IWDG 超时时间（当前配置：~26 秒）
 *
 * 配置参数（当前预留配置，未启用）：
 *   - 预分频器：256（LSI 40kHz / 256 ≈ 156Hz）
 *   - 重载值：4095（最大值）
 *   - 超时时间：4095 / 156 ≈ 26.2 秒
 * ============================================================================= */

#ifndef MONITORING_HW_WATCHDOG_H
#define MONITORING_HW_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化硬件独立看门狗（IWDG）
 * @note   当前版本：MONITOR_HW_IWDG_ENABLED = 0，不启用 IWDG
 * @note   若启用：配置预分频器 256、重载值 4095，超时约 26 秒
 * @note   IWDG 一旦启动无法停止，只能通过系统复位清除
 */
void MonitoringHardwareWatchdog_Init(void);

/**
 * @brief  喂狗（重载 IWDG 计数器）
 * @note   只有 IWDG 已启用时才执行喂狗操作
 * @note   由 watchdog_task 定期（1 秒）调用
 */
void MonitoringHardwareWatchdog_Refresh(void);

/**
 * @brief  查询 IWDG 是否已启用
 * @return 1=已启用，0=未启用
 * @note   Stop 门禁检查会调用此函数，若已启用则拒绝进 Stop
 */
uint8_t MonitoringHardwareWatchdog_IsEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_HW_WATCHDOG_H */
