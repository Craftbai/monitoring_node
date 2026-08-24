/* =============================================================================
 * monitoring_alerts.c - 告警状态机与连续确认策略实现
 *
 * 状态机设计原则：
 *   1. 无效周期不参与状态流转
 *      传感器缺失、采集超时、算法饱和等导致的无效数据，不应被误判为
 *      "恢复正常"，也不应推进确认计数。状态机只响应有效的物理量。
 *
 *   2. 触发阈值与恢复阈值分离（迟滞）
 *      触发阈值 > 恢复阈值，避免在临界点附近抖动（频繁进出告警）。
 *      例如：振动触发 350mg，恢复 300mg，中间 50mg 的死区。
 *
 *   3. 连续确认策略
 *      单次超限不立即进入 ACTIVE，需要连续 N 个有效周期持续超限，
 *      避免瞬态尖峰触发误报。当前配置 N=3（MONITOR_ALERT_CONFIRM_CYCLES）。
 *
 *   4. 连续恢复策略
 *      ACTIVE 状态下单次低于恢复阈值不立即清除，需要连续 M 个有效周期
 *      持续低于恢复阈值，确保故障真正消失。当前配置 M=3（RECOVER_CYCLES）。
 *
 *   5. PENDING 阶段快速清除
 *      PENDING 状态下单次低于恢复阈值立即回 NORMAL，因为此阶段告警
 *      尚未确认，不需要连续恢复周期（避免延迟清除未确认的抖动）。
 *
 * 状态流转表：
 *   当前状态    | 有效 | 超限 | 恢复 | 下一状态      | 计数器操作
 *   ------------|------|------|------|--------------|------------------
 *   NORMAL      | 1    | 1    | X    | PENDING      | over_count = 1
 *   NORMAL      | 1    | 0    | X    | NORMAL       | 不变
 *   NORMAL      | 0    | X    | X    | NORMAL       | 不变（无效周期）
 *   PENDING     | 1    | 1    | 0    | PENDING/ACTIVE | over_count++，达标→ACTIVE
 *   PENDING     | 1    | 0    | 1    | NORMAL       | 清零计数器（快速清除）
 *   PENDING     | 0    | X    | X    | PENDING      | 不变（无效周期）
 *   ACTIVE      | 1    | 1    | 0    | ACTIVE       | 保持
 *   ACTIVE      | 1    | 0    | 1    | RECOVERING   | recovery_count = 1
 *   ACTIVE      | 0    | X    | X    | ACTIVE       | 不变（无效周期）
 *   RECOVERING  | 1    | 1    | 0    | ACTIVE       | 清零恢复计数（重新超限）
 *   RECOVERING  | 1    | 0    | 1    | RECOVERING/NORMAL | recovery_count++，达标→NORMAL
 *   RECOVERING  | 0    | X    | X    | RECOVERING   | 不变（无效周期）
 * ============================================================================= */

#include "monitoring_alerts.h"

#include <string.h>

/* 告警通道总数：温度 + 电流 + 振动3轴 = 5 */
#define MONITOR_ALERT_COUNT       (MONITOR_VIBRATION_AXES + 2U)

/* ===== 静态状态变量 ===== */

/* 各通道的状态机状态（索引：0=温度，1=电流，2/3/4=振动X/Y/Z） */
static monitor_alert_state_t g_states[MONITOR_ALERT_COUNT];

/* 各通道的连续超限计数（PENDING/ACTIVE 阶段使用） */
static uint8_t g_over_count[MONITOR_ALERT_COUNT];

/* 各通道的连续恢复计数（RECOVERING 阶段使用） */
static uint8_t g_recovery_count[MONITOR_ALERT_COUNT];

/* ===== 公开 API 实现 ===== */

/**
 * @brief  复位所有通道的告警状态
 * @note   在 MonitoringTasks_Create() 中调用
 */
void MonitoringAlerts_Reset(void)
{
  memset(g_states, 0, sizeof(g_states));
  memset(g_over_count, 0, sizeof(g_over_count));
  memset(g_recovery_count, 0, sizeof(g_recovery_count));
}

/**
 * @brief  更新单个通道的告警状态
 * @param  index: 通道索引（0=温度，1=电流，2/3/4=振动X/Y/Z）
 * @param  valid: 该通道数据是否有效（1=有效，0=无效）
 * @param  over_limit: 是否超限（1=超限，0=未超限）
 * @param  recovered: 是否低于恢复阈值（1=已恢复，0=未恢复）
 * @param  bit: 该通道在 alert_mask 中的位掩码（1UL << index）
 * @param  result: 周期结果指针（写入告警状态和计数）
 *
 * @note   状态流转逻辑：
 *         - 无效数据：不改变状态，不推进计数（直接返回）
 *         - NORMAL + 超限：进入 PENDING，over_count = 1
 *         - PENDING + 超限：over_count++，达标后进入 ACTIVE
 *         - PENDING + 恢复：立即回 NORMAL，清零计数（快速清除未确认告警）
 *         - ACTIVE + 恢复：进入 RECOVERING，recovery_count = 1
 *         - RECOVERING + 超限：回 ACTIVE，清零恢复计数（重新超限）
 *         - RECOVERING + 恢复：recovery_count++，达标后回 NORMAL
 */
static void MonitoringAlerts_UpdateOne(uint8_t index, uint8_t valid,
                                       uint8_t over_limit, uint8_t recovered,
                                       uint32_t bit,
                                       monitor_cycle_result_t *result)
{
  if (valid == 0U)
  {
    /* 无效数据不改变告警状态，也不被当作恢复周期。 */
    return;
  }

  if (over_limit != 0U)
  {
    g_recovery_count[index] = 0U;
    if (g_over_count[index] < MONITOR_ALERT_CONFIRM_CYCLES)
    {
      g_over_count[index]++;
    }
    if (g_states[index] != MONITOR_ALERT_ACTIVE)
    {
      g_states[index] = MONITOR_ALERT_PENDING;
    }
    if (g_over_count[index] >= MONITOR_ALERT_CONFIRM_CYCLES)
    {
      g_states[index] = MONITOR_ALERT_ACTIVE;
      result->alert_mask |= bit;
    }
  }
  else if (recovered != 0U)
  {
    if (g_states[index] == MONITOR_ALERT_ACTIVE ||
        g_states[index] == MONITOR_ALERT_RECOVERING)
    {
      g_over_count[index] = 0U;
      g_states[index] = MONITOR_ALERT_RECOVERING;
      if (g_recovery_count[index] < MONITOR_ALERT_RECOVER_CYCLES)
      {
        g_recovery_count[index]++;
      }
      if (g_recovery_count[index] >= MONITOR_ALERT_RECOVER_CYCLES)
      {
        g_states[index] = MONITOR_ALERT_NORMAL;
        g_recovery_count[index] = 0U;
      }
    }
    else
    {
      /* PENDING 阶段的单次恢复只结束本轮待确认，不进入恢复态。 */
      g_over_count[index] = 0U;
      g_states[index] = MONITOR_ALERT_NORMAL;
    }
  }
  else if (g_states[index] == MONITOR_ALERT_PENDING)
  {
    /* 尚未达到恢复阈值时，未超限但仍处于迟滞区，清除本次待确认计数。 */
    g_over_count[index] = 0U;
    g_states[index] = MONITOR_ALERT_NORMAL;
  }
}

/**
 * @brief  根据特征值更新所有通道的告警状态
 * @param  result: 周期结果指针（输入特征值，输出告警状态）
 *
 * @note   更新流程：
 *         1. 对每个通道调用 MonitoringAlerts_UpdateOne：
 *            - 温度：阈值 MONITOR_TEMP_LIMIT_CENTI / RECOVER_CENTI
 *            - 电流：阈值 MONITOR_CURRENT_LIMIT_MA / RECOVER_MA
 *            - 振动X/Y/Z：阈值 MONITOR_VIBRATION_LIMIT_MG / RECOVER_MG
 *         2. 汇总所有通道的状态到 result->alert_states（每通道 2 bit）
 *         3. 汇总所有 ACTIVE 通道到 result->alert_mask（每通道 1 bit）
 *         4. 汇总所有通道的计数到 result->alert_counts
 *
 * @note   输出格式：
 *         - alert_states: 32 位，每通道 2 bit（00=NORMAL, 01=PENDING, 10=ACTIVE, 11=RECOVERING）
 *         - alert_mask: 32 位，每通道 1 bit（1=ACTIVE，0=非ACTIVE）
 *         - alert_counts[5]: 每通道的计数（PENDING/ACTIVE 时为 over_count，RECOVERING 时为 recovery_count）
 */
void MonitoringAlerts_Update(monitor_cycle_result_t *result)
{
  uint8_t valid;

  if (result == NULL)
  {
    return;
  }

  valid = (result->valid_mask & MONITOR_VALID_TEMPERATURE) != 0U;
  MonitoringAlerts_UpdateOne(0U, valid,
    valid != 0U && result->temperature_centi > MONITOR_TEMP_LIMIT_CENTI,
    valid != 0U && result->temperature_centi <= MONITOR_TEMP_RECOVER_CENTI,
    1UL << 0, result);

  valid = (result->valid_mask & MONITOR_VALID_CURRENT) != 0U;
  MonitoringAlerts_UpdateOne(1U, valid,
    valid != 0U && result->current_milliamp > MONITOR_CURRENT_LIMIT_MA,
    valid != 0U && result->current_milliamp <= MONITOR_CURRENT_RECOVER_MA,
    1UL << 1, result);

  for (uint8_t axis = 0U; axis < MONITOR_VIBRATION_AXES; axis++)
  {
    valid = (result->valid_mask & MONITOR_VALID_VIBRATION) != 0U;
    MonitoringAlerts_UpdateOne((uint8_t)(axis + 2U), valid,
      valid != 0U && result->vibration_rms_mg[axis] > MONITOR_VIBRATION_LIMIT_MG,
      valid != 0U && result->vibration_rms_mg[axis] <= MONITOR_VIBRATION_RECOVER_MG,
      1UL << (axis + 2U), result);
  }

  result->alert_states = 0U;
  for (uint8_t i = 0U; i < MONITOR_ALERT_COUNT; i++)
  {
    result->alert_states |= ((uint32_t)g_states[i] & 0x03UL) << (i * 2U);
    if (g_states[i] == MONITOR_ALERT_ACTIVE)
    {
      result->alert_mask |= 1UL << i;
    }
    result->alert_counts[i] =
      (g_states[i] == MONITOR_ALERT_ACTIVE ||
       g_states[i] == MONITOR_ALERT_PENDING) ? g_over_count[i] :
      (g_states[i] == MONITOR_ALERT_RECOVERING ? g_recovery_count[i] : 0U);
  }
}
