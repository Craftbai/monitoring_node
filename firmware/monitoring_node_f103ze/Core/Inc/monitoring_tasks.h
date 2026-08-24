#ifndef MONITORING_TASKS_H
#define MONITORING_TASKS_H

#include "cmsis_os2.h"
#include "monitoring_config.h"
/* 必须包含完整 HAL 头，不能直接包含 hal_def，避免 F1 设备头循环包含。 */
#include "stm32f1xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固件配置：测试模式便于串口观察，正式模式使用低功耗周期。 */
#define MONITOR_CONFIG_VERSION       0x00010001UL
#define MONITOR_SAMPLE_POOL_SIZE     2U
#define MONITOR_VIBRATION_AXES       3U
#define MONITOR_VIBRATION_SAMPLES    1024U
#define MONITOR_CURRENT_SAMPLES      1024U
#define MONITOR_QUEUE_DEPTH          2U
#define MONITOR_TEST_MODE            0U
#define MONITOR_TEST_INTERVAL_SEC    5U
#define MONITOR_PRODUCTION_PERIOD_SEC 300U
#if MONITOR_TEST_MODE
#define MONITOR_CYCLE_INTERVAL_SEC   MONITOR_TEST_INTERVAL_SEC
#else
#define MONITOR_CYCLE_INTERVAL_SEC   MONITOR_PRODUCTION_PERIOD_SEC
#endif
#define MONITOR_CYCLE_WAIT_TIMEOUT_MS ((MONITOR_CYCLE_INTERVAL_SEC + 10U) * 1000U)
#define MONITOR_REPORT_WAIT_TIMEOUT_MS 12000U
#define MONITOR_EVENT_RTC_ALARM      (1UL << 0)
#define MONITOR_EVENT_CAPTURE        (1UL << 1)
#define MONITOR_EVENT_REPORT_DONE    (1UL << 2)
#define MONITOR_EVENT_STOP_WAKE      (1UL << 3)

/* 采集块池和空闲队列必须一一对应，否则可能出现块永久丢失。 */
#if MONITOR_SAMPLE_POOL_SIZE > MONITOR_QUEUE_DEPTH
#error "MONITOR_QUEUE_DEPTH must cover MONITOR_SAMPLE_POOL_SIZE"
#endif

#define MONITOR_SAMPLE_FLAG_NO_SENSOR       (1UL << 0)
#define MONITOR_SAMPLE_FLAG_TEMP_INVALID    (1UL << 1)
#define MONITOR_SAMPLE_FLAG_VIB_INVALID     (1UL << 2)
#define MONITOR_SAMPLE_FLAG_CURRENT_INVALID (1UL << 3)
#define MONITOR_SAMPLE_FLAG_TIMEOUT         (1UL << 4)
#define MONITOR_SAMPLE_FLAG_OVERFLOW        (1UL << 5)
#define MONITOR_SAMPLE_FLAG_SATURATION      (1UL << 6)
#define MONITOR_SAMPLE_FLAG_TEMP_MISSING    (1UL << 7)
#define MONITOR_SAMPLE_FLAG_VIB_MISSING     (1UL << 8)
#define MONITOR_SAMPLE_FLAG_CURRENT_MISSING (1UL << 9)

#define MONITOR_ERROR_NO_SENSOR             (1UL << 0)
#define MONITOR_ERROR_ACQUIRE_DROP          (1UL << 1)
#define MONITOR_ERROR_PROCESS_DROP          (1UL << 2)
#define MONITOR_ERROR_REPORT_DROP           (1UL << 3)
#define MONITOR_ERROR_TEMP                  (1UL << 4)
#define MONITOR_ERROR_VIBRATION             (1UL << 5)
#define MONITOR_ERROR_CURRENT               (1UL << 6)
#define MONITOR_ERROR_ALGORITHM             (1UL << 7)
#define MONITOR_ERROR_STOP_GATE             (1UL << 8)
#define MONITOR_ERROR_WATCHDOG              (1UL << 9)

#define MONITOR_VALID_TEMPERATURE           (1UL << 0)
#define MONITOR_VALID_VIBRATION             (1UL << 1)
#define MONITOR_VALID_CURRENT               (1UL << 2)

typedef enum
{
  MONITOR_STATE_BOOT = 0,
  MONITOR_STATE_SELF_TEST,
  MONITOR_STATE_IDLE,
  MONITOR_STATE_ACQUIRE,
  MONITOR_STATE_PROCESS,
  MONITOR_STATE_EVALUATE,
  MONITOR_STATE_REPORT,
  MONITOR_STATE_PREPARE_STOP,
  MONITOR_STATE_STOP,
  MONITOR_STATE_FAULT
} monitor_state_t;

typedef struct
{
  uint32_t cycle_id;
  uint32_t timestamp_ticks;
} monitor_cycle_request_t;

typedef struct
{
  uint32_t cycle_id;
  uint32_t sequence;
  uint32_t timestamp_ticks;
  uint32_t flags;
  uint32_t channel_mask;
  uint16_t vibration_rate_hz;
  uint16_t vibration_sample_count;
  uint16_t current_rate_hz;
  uint16_t current_sample_count;
  int16_t temperature_centi;
  uint16_t current_raw;
  int16_t vibration[MONITOR_VIBRATION_AXES][MONITOR_VIBRATION_SAMPLES];
  uint16_t current[MONITOR_CURRENT_SAMPLES];
} monitor_sample_block_t;

typedef struct
{
  uint32_t cycle_id;
  uint32_t config_version;
  uint32_t valid_mask;
  uint32_t error_flags;
  uint32_t sample_flags;
  uint16_t vibration_sample_count;
  uint16_t current_sample_count;
  uint16_t vibration_rate_hz;
  uint16_t current_rate_hz;
  int16_t temperature_centi;
  uint16_t current_mean_raw;
  uint16_t current_rms_raw;
  int32_t current_milliamp;
  int32_t vibration_rms_mg[MONITOR_VIBRATION_AXES];
  int32_t vibration_peak_to_peak_mg[MONITOR_VIBRATION_AXES];
  uint32_t vibration_crest_milli[MONITOR_VIBRATION_AXES];
  uint32_t vibration_band_energy_milli[MONITOR_VIBRATION_AXES];
  uint32_t alert_mask;
  uint32_t alert_states;
  uint8_t alert_counts[MONITOR_VIBRATION_AXES + 2U];
  uint32_t algorithm_saturation_count;
  uint32_t processing_time_ms;
  monitor_state_t state;
} monitor_cycle_result_t;

typedef struct
{
  volatile uint32_t cycles_started;
  volatile uint32_t samples_ready;
  volatile uint32_t samples_processed;
  volatile uint32_t reports_sent;
  volatile uint32_t acquire_drops;
  volatile uint32_t process_drops;
  volatile uint32_t report_drops;
  volatile uint32_t rtc_wait_timeouts;
  volatile uint32_t rtc_alarm_errors;
  volatile uint32_t dma_half_events;
  volatile uint32_t dma_full_events;
  volatile uint32_t sensor_errors;
  volatile uint32_t watchdog_faults;
  volatile uint32_t stop_entries;
  volatile uint32_t stop_wakeups;
  volatile uint32_t acquisition_heartbeat;
  volatile uint32_t processing_heartbeat;
  volatile uint32_t report_heartbeat;
  volatile uint32_t nrf24_reports;
  volatile uint32_t nrf24_errors;
  volatile uint16_t acquisition_stack_free_words;
  volatile uint16_t processing_stack_free_words;
  volatile uint16_t report_stack_free_words;
  volatile uint16_t health_stack_free_words;
  volatile uint16_t cycle_stack_free_words;
  volatile uint16_t watchdog_stack_free_words;
  volatile monitor_state_t state;
} monitor_health_stats_t;

/* FreeRTOS 任务链路的 RTC Alarm 事件标志。 */
extern osEventFlagsId_t g_monitor_cycle_event;

/* 软件看门狗和低功耗模块使用的事件接口。 */
extern osEventFlagsId_t g_monitor_control_event;

/* RTC 闹钟设置函数由 main.c 提供，返回值必须由调用者检查。 */
HAL_StatusTypeDef RTC_SetNextAlarm(uint32_t interval_sec);

/* 在 MX_FREERTOS_Init 的 USER CODE 区创建固定队列、块池和应用任务。 */
void MonitoringTasks_Create(void);

/* 自定义静态 cycle_task 作为周期协调任务入口；defaultTask 只负责 LED 心跳。 */
void MonitoringTasks_RunCycleTask(void *argument);

/* 中断和驱动层只发布事件，不直接运行算法或日志。 */
void MonitoringTasks_OnMpuDataReady(void);
void MonitoringTasks_OnAdcHalfComplete(void);
void MonitoringTasks_OnAdcComplete(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_TASKS_H */
