/* =============================================================================
 * monitoring_alerts.h - 告警状态机与连续确认策略
 *
 * 职责：根据特征值与阈值对比结果，管理告警状态流转，实现连续确认和
 *       连续恢复策略，避免单次抖动触发误报。
 *
 * 状态机流转：
 *   NORMAL（正常）
 *     ↓ 连续 N 个有效周期超限
 *   PENDING（待确认）
 *     ↓ 达到确认周期数（N）
 *   ACTIVE（激活）
 *     ↓ 连续 M 个有效周期低于恢复阈值
 *   RECOVERING（恢复中）
 *     ↓ 达到恢复周期数（M）
 *   NORMAL（正常）
 *
 * 关键策略：
 *   - 无效周期（传感器缺失、采集失败）不改变告警状态
 *   - 触发阈值与恢复阈值分离，形成迟滞（避免临界点抖动）
 *   - 连续确认：N 个有效周期持续超限后才进入 ACTIVE
 *   - 连续恢复：M 个有效周期持续低于恢复阈值后才清除告警
 *   - PENDING 阶段单次恢复直接回 NORMAL（未确认的告警快速清除）
 * ============================================================================= */

#ifndef MONITORING_ALERTS_H
#define MONITORING_ALERTS_H

#include "monitoring_tasks.h"

/**
 * @brief  告警状态枚举
 */
typedef enum
{
  MONITOR_ALERT_NORMAL = 0,      /* 正常状态，未超限 */
  MONITOR_ALERT_PENDING,         /* 待确认，已超限但未达到连续确认周期数 */
  MONITOR_ALERT_ACTIVE,          /* 激活状态，连续超限已确认 */
  MONITOR_ALERT_RECOVERING       /* 恢复中，已低于恢复阈值但未达到连续恢复周期数 */
} monitor_alert_state_t;

/**
 * @brief  复位所有通道的告警状态
 * @note   在 MonitoringTasks_Create() 中调用，确保启动时所有通道为 NORMAL
 * @note   复位内容：状态机、超限计数、恢复计数
 */
void MonitoringAlerts_Reset(void);

/**
 * @brief  根据特征值更新告警状态
 * @param  result: 周期结果指针（输入特征值，输出告警状态）
 *
 * @note   更新流程：
 *         1. 对每个通道（温度/电流/振动X/Y/Z）调用 UpdateOne
 *         2. 判断有效标志、超限状态、恢复状态
 *         3. 更新状态机和计数器
 *         4. 汇总所有通道的状态到 result->alert_states（每通道 2 bit）
 *         5. 汇总所有 ACTIVE 通道到 result->alert_mask（每通道 1 bit）
 *         6. 汇总所有通道的计数到 result->alert_counts
 *
 * @note   通道索引：
 *         0: 温度
 *         1: 电流
 *         2: 振动 X
 *         3: 振动 Y
 *         4: 振动 Z
 */
void MonitoringAlerts_Update(monitor_cycle_result_t *result);

#endif /* MONITORING_ALERTS_H */
