#include "monitoring_alerts.h"

#include <string.h>

#define MONITOR_ALERT_COUNT       (MONITOR_VIBRATION_AXES + 2U)

/* 0:温度，1:电流，2/3/4:振动 X/Y/Z。 */
static monitor_alert_state_t g_states[MONITOR_ALERT_COUNT];
static uint8_t g_over_count[MONITOR_ALERT_COUNT];
static uint8_t g_recovery_count[MONITOR_ALERT_COUNT];

void MonitoringAlerts_Reset(void)
{
  memset(g_states, 0, sizeof(g_states));
  memset(g_over_count, 0, sizeof(g_over_count));
  memset(g_recovery_count, 0, sizeof(g_recovery_count));
}

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
