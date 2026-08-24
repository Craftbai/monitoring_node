#ifndef MONITORING_ALERTS_H
#define MONITORING_ALERTS_H

#include "monitoring_tasks.h"

typedef enum
{
  MONITOR_ALERT_NORMAL = 0,
  MONITOR_ALERT_PENDING,
  MONITOR_ALERT_ACTIVE,
  MONITOR_ALERT_RECOVERING
} monitor_alert_state_t;

void MonitoringAlerts_Reset(void);
void MonitoringAlerts_Update(monitor_cycle_result_t *result);

#endif /* MONITORING_ALERTS_H */
