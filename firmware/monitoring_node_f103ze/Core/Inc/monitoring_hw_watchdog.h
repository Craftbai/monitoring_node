#ifndef MONITORING_HW_WATCHDOG_H
#define MONITORING_HW_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件 IWDG 默认关闭；启用前必须先解决 Stop 期间的计时策略。 */
void MonitoringHardwareWatchdog_Init(void);
void MonitoringHardwareWatchdog_Refresh(void);
uint8_t MonitoringHardwareWatchdog_IsEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_HW_WATCHDOG_H */
