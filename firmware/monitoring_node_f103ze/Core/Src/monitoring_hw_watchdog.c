#include "monitoring_hw_watchdog.h"

#include "monitoring_config.h"
#include "stm32f1xx_hal.h"

#define MONITOR_IWDG_KEY_ENABLE       0xCCCCU
#define MONITOR_IWDG_KEY_RELOAD       0xAAAAU
#define MONITOR_IWDG_KEY_WRITE        0x5555U
#define MONITOR_IWDG_PRESCALER_256    6U
#define MONITOR_IWDG_RELOAD_MAX       0x0FFFU

static uint8_t g_iwdg_enabled;

void MonitoringHardwareWatchdog_Init(void)
{
  g_iwdg_enabled = 0U;

#if MONITOR_HW_IWDG_ENABLED
  /* 仅提供可控的初始化入口，实际 Stop 策略由上层门禁保证。 */
  IWDG->KR = MONITOR_IWDG_KEY_WRITE;
  IWDG->PR = MONITOR_IWDG_PRESCALER_256;
  IWDG->RLR = MONITOR_IWDG_RELOAD_MAX;
  IWDG->KR = MONITOR_IWDG_KEY_RELOAD;
  IWDG->KR = MONITOR_IWDG_KEY_ENABLE;
  g_iwdg_enabled = 1U;
#endif
}

void MonitoringHardwareWatchdog_Refresh(void)
{
  if (g_iwdg_enabled != 0U)
  {
    IWDG->KR = MONITOR_IWDG_KEY_RELOAD;
  }
}

uint8_t MonitoringHardwareWatchdog_IsEnabled(void)
{
  return g_iwdg_enabled;
}
