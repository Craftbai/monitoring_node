#include "monitoring_tasks.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "task.h"
#include "main.h"
#include "usart.h"
#include "rtc.h"
#include "monitoring_acquisition.h"
#include "monitoring_algorithm.h"
#include "monitoring_alerts.h"
#include "monitoring_spi.h"
#include "monitoring_nrf24.h"
#include "monitoring_hw_watchdog.h"

#include <string.h>

/* UART_Log 位于 main.c；任务上下文中允许低速、阻塞式诊断输出。 */
extern void UART_Log(const char *fmt, ...);

#define MONITOR_HEALTH_PERIOD_MS 10000U
#define MONITOR_WATCHDOG_TIMEOUT_MS 15000U

/* Stop 唤醒后由 main.c 提供系统时钟恢复函数。 */
extern void SystemClock_Config(void);

static void MonitoringTasks_Nrf24ChipSelect(uint8_t active)
{
  HAL_GPIO_WritePin(NRF24_CSN_GPIO_Port, NRF24_CSN_Pin,
                    active != 0U ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void MonitoringTasks_Nrf24ChipEnable(uint8_t active)
{
  HAL_GPIO_WritePin(NRF24_CE_GPIO_Port, NRF24_CE_Pin,
                    active != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

#if MONITOR_NRF24_REPORT_ENABLED
static uint8_t MonitoringTasks_PutU32(uint8_t *buffer, uint8_t offset,
                                      uint32_t value)
{
  buffer[offset++] = (uint8_t)(value & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 8U) & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 16U) & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 24U) & 0xFFU);
  return offset;
}

static uint8_t MonitoringTasks_BuildNrf24Payload(
  const monitor_cycle_result_t *result, uint8_t *payload)
{
  uint8_t offset = 0U;

  /* 当前使用固定长度管道，载荷补齐到 RX_PW_P0 配置的 32 字节，
   * 让接收端始终按同一个帧长度读取。 */
  memset(payload, 0, MONITOR_NRF24_PAYLOAD_SIZE);
  offset = MonitoringTasks_PutU32(payload, offset, result->cycle_id);
  offset = MonitoringTasks_PutU32(payload, offset, result->valid_mask);
  offset = MonitoringTasks_PutU32(payload, offset, result->error_flags);
  payload[offset++] = (uint8_t)(result->temperature_centi & 0xFF);
  payload[offset++] = (uint8_t)((uint16_t)result->temperature_centi >> 8U);
  offset = MonitoringTasks_PutU32(payload, offset, (uint32_t)result->current_milliamp);
  for (uint8_t axis = 0U; axis < MONITOR_VIBRATION_AXES; axis++)
  {
    payload[offset++] = (uint8_t)(result->vibration_rms_mg[axis] & 0xFF);
    payload[offset++] = (uint8_t)((uint32_t)result->vibration_rms_mg[axis] >> 8U);
  }
  offset = MonitoringTasks_PutU32(payload, offset, result->alert_mask);
  payload[offset++] = (uint8_t)result->state;
  return MONITOR_NRF24_PAYLOAD_SIZE;
}
#endif

/* 固定块池：采集任务取得所有权，处理任务归还所有权。 */
static monitor_sample_block_t g_sample_pool[MONITOR_SAMPLE_POOL_SIZE];
static uint32_t g_sample_sequence = 0U;

static osMessageQueueId_t g_cycle_request_queue;
static osMessageQueueId_t g_sample_free_queue;
static osMessageQueueId_t g_sample_ready_queue;
static osMessageQueueId_t g_result_queue;
osEventFlagsId_t g_monitor_cycle_event;
osEventFlagsId_t g_monitor_control_event;

static StaticQueue_t g_cycle_request_queue_cb;
static StaticQueue_t g_sample_free_queue_cb;
static StaticQueue_t g_sample_ready_queue_cb;
static StaticQueue_t g_result_queue_cb;
static StaticEventGroup_t g_cycle_event_cb;
static StaticEventGroup_t g_control_event_cb;
static StaticSemaphore_t g_report_done_sem_cb;

static uint8_t g_cycle_request_queue_mem[sizeof(monitor_cycle_request_t) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_sample_free_queue_mem[sizeof(monitor_sample_block_t *) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_sample_ready_queue_mem[sizeof(monitor_sample_block_t *) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_result_queue_mem[sizeof(monitor_cycle_result_t) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));

static StaticTask_t g_acquisition_task_cb;
static StaticTask_t g_processing_task_cb;
static StaticTask_t g_report_task_cb;
static StaticTask_t g_health_task_cb;
static StaticTask_t g_cycle_task_cb;
static StaticTask_t g_watchdog_task_cb;

static StackType_t g_acquisition_task_stack[256];
static StackType_t g_processing_task_stack[256];
static StackType_t g_report_task_stack[192];
static StackType_t g_health_task_stack[192];
static StackType_t g_cycle_task_stack[256];
static StackType_t g_watchdog_task_stack[160];

static osThreadId_t g_acquisition_task;
static osThreadId_t g_processing_task;
static osThreadId_t g_report_task;
static osThreadId_t g_health_task;
static osThreadId_t g_cycle_task;
static osThreadId_t g_watchdog_task;
static osMutexId_t g_log_mutex;
static osSemaphoreId_t g_report_done_sem;
static StaticSemaphore_t g_log_mutex_cb;
static volatile uint8_t g_watchdog_permit;
static uint32_t g_watchdog_last_acquisition;
static uint32_t g_watchdog_last_processing;
static uint32_t g_watchdog_last_report;
static uint32_t g_watchdog_last_progress_tick;

static uint8_t MonitoringTasks_LogLock(void)
{
  if (g_log_mutex != NULL && osKernelGetState() == osKernelRunning)
  {
    return (osMutexAcquire(g_log_mutex, 100U) == osOK) ? 1U : 0U;
  }

  return 0U;
}

static void MonitoringTasks_LogUnlock(void)
{
  if (g_log_mutex != NULL && osKernelGetState() == osKernelRunning)
  {
    (void)osMutexRelease(g_log_mutex);
  }
}

#define MONITOR_LOG(...) do { \
  uint8_t monitor_log_locked = MonitoringTasks_LogLock(); \
  UART_Log(__VA_ARGS__); \
  if (monitor_log_locked != 0U) { MonitoringTasks_LogUnlock(); } \
} while (0)
static monitor_health_stats_t g_health = {
  .cycles_started = 0U,
  .samples_ready = 0U,
  .samples_processed = 0U,
  .reports_sent = 0U,
  .acquire_drops = 0U,
  .process_drops = 0U,
  .report_drops = 0U,
  .rtc_wait_timeouts = 0U,
  .rtc_alarm_errors = 0U,
  .state = MONITOR_STATE_BOOT
};

static const char *MonitoringTasks_StateText(monitor_state_t state)
{
  switch (state)
  {
    case MONITOR_STATE_BOOT:         return "BOOT";
    case MONITOR_STATE_SELF_TEST:    return "SELF_TEST";
    case MONITOR_STATE_IDLE:         return "IDLE";
    case MONITOR_STATE_ACQUIRE:      return "ACQUIRE";
    case MONITOR_STATE_PROCESS:      return "PROCESS";
    case MONITOR_STATE_EVALUATE:     return "EVALUATE";
    case MONITOR_STATE_REPORT:       return "REPORT";
    case MONITOR_STATE_PREPARE_STOP: return "PREPARE_STOP";
    case MONITOR_STATE_STOP:         return "STOP";
    case MONITOR_STATE_FAULT:        return "FAULT";
    default:                         return "UNKNOWN";
  }
}

static osMessageQueueId_t MonitoringTasks_CreateQueue(uint32_t count,
                                                       uint32_t item_size,
                                                       StaticQueue_t *control_block,
                                                       void *storage,
                                                       uint32_t storage_size,
                                                       const char *name)
{
  const osMessageQueueAttr_t attributes = {
    .name = name,
    .cb_mem = control_block,
    .cb_size = sizeof(*control_block),
    .mq_mem = storage,
    .mq_size = storage_size
  };

  return osMessageQueueNew(count, item_size, &attributes);
}

static osThreadId_t MonitoringTasks_CreateThread(const char *name,
                                                  osThreadFunc_t function,
                                                  osPriority_t priority,
                                                  StaticTask_t *control_block,
                                                  StackType_t *stack,
                                                  uint32_t stack_words)
{
  const osThreadAttr_t attributes = {
    .name = name,
    .cb_mem = control_block,
    .cb_size = sizeof(*control_block),
    .stack_mem = stack,
    .stack_size = stack_words * sizeof(StackType_t),
    .priority = priority
  };

  return osThreadNew(function, NULL, &attributes);
}

/* 闹钟重设集中处理，避免异常路径漏掉下一次 RTC 事件。 */
static void MonitoringTasks_RearmRtcAlarm(const char *reason)
{
  if (RTC_SetNextAlarm(MONITOR_CYCLE_INTERVAL_SEC) != HAL_OK)
  {
    g_health.rtc_alarm_errors++;
    MONITOR_LOG("[RTOS] RTC alarm rearm failed, reason=%s\r\n", reason);
  }
}

static uint8_t MonitoringTasks_EnterStop(void)
{
  if (g_health.state == MONITOR_STATE_FAULT || g_watchdog_permit == 0U)
  {
    g_health.state = MONITOR_STATE_FAULT;
    return 0U;
  }

  /* IWDG 在 Stop 中可能继续计时，未完成独立的低功耗喂狗策略时拒绝休眠。 */
  if (MonitoringHardwareWatchdog_IsEnabled() != 0U)
  {
    g_health.state = MONITOR_STATE_IDLE;
    MONITOR_LOG("[RTOS] stop gate denied; hardware watchdog is enabled\r\n");
    return 0U;
  }

  if (osMessageQueueGetCount(g_cycle_request_queue) != 0U ||
      osMessageQueueGetCount(g_sample_ready_queue) != 0U ||
      osMessageQueueGetCount(g_result_queue) != 0U ||
      osMessageQueueGetCount(g_sample_free_queue) != MONITOR_SAMPLE_POOL_SIZE)
  {
    /* 队列瞬时未清空时继续运行，交给下一周期重新尝试 Stop。 */
    g_health.state = MONITOR_STATE_IDLE;
    MONITOR_LOG("[RTOS] stop gate busy; keep running, q=%lu/%lu/%lu free=%lu\r\n",
                (unsigned long)osMessageQueueGetCount(g_cycle_request_queue),
                (unsigned long)osMessageQueueGetCount(g_sample_ready_queue),
                (unsigned long)osMessageQueueGetCount(g_result_queue),
                (unsigned long)osMessageQueueGetCount(g_sample_free_queue));
    return 0U;
  }

  g_health.state = MONITOR_STATE_PREPARE_STOP;
  if (MonitoringAcquisition_Stop() == 0U)
  {
    g_health.state = MONITOR_STATE_FAULT;
    g_watchdog_permit = 0U;
    g_health.rtc_alarm_errors++;
    MONITOR_LOG("[RTOS] acquisition stop failed; stop denied\r\n");
    return 0U;
  }

  /* 采集已停止、队列已清空后才布置一次性闹钟，避免失败路径留下闹钟。 */
  if (RTC_SetNextAlarm(MONITOR_CYCLE_INTERVAL_SEC) != HAL_OK)
  {
    g_health.rtc_alarm_errors++;
    g_health.state = MONITOR_STATE_FAULT;
    MONITOR_LOG("[RTOS] stop alarm setup failed; stop denied\r\n");
    return 0U;
  }

  MonitoringNrf24_Sleep();
  HAL_GPIO_WritePin(SENSOR_POWER_EN_GPIO_Port, SENSOR_POWER_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TRACE_PIN_GPIO_Port, TRACE_PIN_Pin, GPIO_PIN_RESET);
  g_health.stop_entries++;
  g_health.state = MONITOR_STATE_STOP;

  /* 闹钟已在 Stop 门禁通过后布置；Stop 中不打印日志，避免阻塞唤醒路径。 */
  HAL_SuspendTick();
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
  SystemClock_Config();
  /* Stop 会关闭运行时钟，先恢复 TIM4 的 HAL 毫秒节拍，再等待 RTC RSF。
   * 否则 HAL_RTC_WaitForSynchro 的超时计数可能停在原值。 */
  HAL_ResumeTick();
  if (RTC_WaitForSync() != HAL_OK)
  {
    g_health.state = MONITOR_STATE_FAULT;
    g_health.rtc_alarm_errors++;
    return 0U;
  }
  HAL_GPIO_WritePin(SENSOR_POWER_EN_GPIO_Port, SENSOR_POWER_EN_Pin, GPIO_PIN_SET);
  HAL_Delay(10U);
  if (MonitoringAcquisition_Resume() == 0U)
  {
    g_health.state = MONITOR_STATE_FAULT;
    g_watchdog_permit = 0U;
    g_health.rtc_alarm_errors++;
    return 0U;
  }
  (void)MonitoringNrf24_Init();
  g_health.stop_wakeups++;
  g_watchdog_last_progress_tick = HAL_GetTick();
  g_health.state = MONITOR_STATE_IDLE;
  return 1U;
}

/*
 * 采集块只允许沿着 free -> ready -> free 的方向流转。
 * 这里集中处理归还失败，避免异常时块从池中静默消失，导致后续周期一直拿不到块。
 */
static uint8_t MonitoringTasks_ReturnSampleBlock(monitor_sample_block_t **block)
{
  if (block == NULL || *block == NULL)
  {
    g_health.acquire_drops++;
    g_health.state = MONITOR_STATE_FAULT;
    return 0U;
  }

  if (osMessageQueuePut(g_sample_free_queue, block, 0U, 0U) != osOK)
  {
    g_health.acquire_drops++;
    g_health.state = MONITOR_STATE_FAULT;
    return 0U;
  }

  return 1U;
}

static void MonitoringTasks_AcquisitionTask(void *argument)
{
  monitor_cycle_request_t request;
  monitor_sample_block_t *block;
  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(g_cycle_request_queue, &request, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    g_health.state = MONITOR_STATE_ACQUIRE;
    if (osMessageQueueGet(g_sample_free_queue, &block, NULL, 100U) != osOK)
    {
      g_health.acquire_drops++;
      g_health.state = MONITOR_STATE_FAULT;
      continue;
    }

    memset(block, 0, sizeof(*block));
    block->cycle_id = request.cycle_id;
    block->sequence = ++g_sample_sequence;
    block->timestamp_ticks = request.timestamp_ticks;
    block->channel_mask = MONITOR_VALID_TEMPERATURE |
                          MONITOR_VALID_VIBRATION |
                          MONITOR_VALID_CURRENT;

    MonitoringAcquisition_Capture(block);
    if ((block->flags & MONITOR_SAMPLE_FLAG_NO_SENSOR) != 0U)
    {
      g_health.sensor_errors++;
    }
    g_health.acquisition_heartbeat++;

    if (osMessageQueuePut(g_sample_ready_queue, &block, 0U, 100U) != osOK)
    {
      g_health.acquire_drops++;
      MonitoringTasks_ReturnSampleBlock(&block);
      g_health.state = MONITOR_STATE_FAULT;
      continue;
    }

    g_health.samples_ready++;
    g_health.state = MONITOR_STATE_IDLE;
  }
}

static void MonitoringTasks_ProcessingTask(void *argument)
{
  monitor_sample_block_t *block;
  monitor_cycle_result_t result;
  uint32_t start_ticks;
  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(g_sample_ready_queue, &block, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    g_health.state = MONITOR_STATE_PROCESS;
    start_ticks = HAL_GetTick();
    memset(&result, 0, sizeof(result));
    result.cycle_id = block->cycle_id;
    result.config_version = MONITOR_CONFIG_VERSION;
    result.state = MONITOR_STATE_EVALUATE;
    MonitoringAlgorithm_Process(block, &result);
    result.processing_time_ms = HAL_GetTick() - start_ticks;
    g_health.processing_heartbeat++;

    if (osMessageQueuePut(g_result_queue, &result, 0U, 100U) != osOK)
    {
      /* 处理已经完成，但结果队列满，归类为上报侧丢弃。 */
      g_health.report_drops++;
    }
    else
    {
      g_health.samples_processed++;
    }

    if (MonitoringTasks_ReturnSampleBlock(&block) != 0U)
    {
      g_health.state = MONITOR_STATE_IDLE;
    }
  }
}

static void MonitoringTasks_ReportTask(void *argument)
{
  monitor_cycle_result_t result;
  int32_t absolute_temperature;
#if MONITOR_NRF24_REPORT_ENABLED
  uint8_t nrf24_payload[MONITOR_NRF24_PAYLOAD_SIZE];
  uint8_t nrf24_payload_length;
  monitoring_nrf24_status_t nrf24_status;
#endif
  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(g_result_queue, &result, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    g_health.state = MONITOR_STATE_REPORT;
    absolute_temperature = result.temperature_centi < 0
                             ? -(int32_t)result.temperature_centi
                             : result.temperature_centi;
    MONITOR_LOG("[RTOS] cycle=%lu state=%s valid=0x%08lx errors=0x%08lx sample=0x%08lx temp=%s%ld.%02ld current=%ldmA vib=%ld/%ld/%ld alert=0x%08lx alert_state=0x%08lx count=%u/%u/%u/%u/%u process_ms=%lu\r\n",
             (unsigned long)result.cycle_id,
             MonitoringTasks_StateText(result.state),
              (unsigned long)result.valid_mask,
              (unsigned long)result.error_flags,
              (unsigned long)result.sample_flags,
             result.temperature_centi < 0 ? "-" : "",
             (long)(absolute_temperature / 100),
             (long)(absolute_temperature % 100),
             (long)result.current_milliamp,
             (long)result.vibration_rms_mg[0],
             (long)result.vibration_rms_mg[1],
              (long)result.vibration_rms_mg[2],
              (unsigned long)result.alert_mask,
              (unsigned long)result.alert_states,
              (unsigned int)result.alert_counts[0],
              (unsigned int)result.alert_counts[1],
              (unsigned int)result.alert_counts[2],
              (unsigned int)result.alert_counts[3],
               (unsigned int)result.alert_counts[4],
               (unsigned long)result.processing_time_ms);
#if MONITOR_NRF24_REPORT_ENABLED
     if (MonitoringNrf24_IsReady() != 0U)
     {
       nrf24_payload_length = MonitoringTasks_BuildNrf24Payload(
         &result, nrf24_payload);
       nrf24_status = MonitoringNrf24_SendPayload(
         nrf24_payload, nrf24_payload_length);
       if (nrf24_status != MONITOR_NRF24_OK)
       {
         g_health.nrf24_errors++;
       }
       else
       {
         g_health.nrf24_reports++;
       }
     }
#endif
     g_health.reports_sent++;
    g_health.report_heartbeat++;
    if (g_report_done_sem != NULL)
    {
      (void)osSemaphoreRelease(g_report_done_sem);
    }
    g_health.state = MONITOR_STATE_IDLE;
  }
}

static void MonitoringTasks_HealthTask(void *argument)
{
  monitoring_spi_status_t spi_status;
  (void)argument;

  for (;;)
  {
    MonitoringSpi_GetStatus(&spi_status);
    g_health.acquisition_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_acquisition_task);
    g_health.processing_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_processing_task);
    g_health.report_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_report_task);
    g_health.health_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_health_task);
    g_health.cycle_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_cycle_task);
    g_health.watchdog_stack_free_words =
      (uint16_t)uxTaskGetStackHighWaterMark((TaskHandle_t)g_watchdog_task);
    MONITOR_LOG("[HEALTH] state=%s cycles=%lu ready=%lu processed=%lu reports=%lu drops=%lu/%lu/%lu rtc=%lu/%lu dma=%lu/%lu sensor=%lu spi=%lu/%lu/%lu/%lu nrf=%lu/%lu wd=%lu permit=%u stack=%u/%u/%u/%u/%u/%u stop=%lu/%lu q=%lu/%lu/%lu\r\n",
             MonitoringTasks_StateText(g_health.state),
             (unsigned long)g_health.cycles_started,
             (unsigned long)g_health.samples_ready,
             (unsigned long)g_health.samples_processed,
             (unsigned long)g_health.reports_sent,
             (unsigned long)g_health.acquire_drops,
             (unsigned long)g_health.process_drops,
             (unsigned long)g_health.report_drops,
             (unsigned long)g_health.rtc_wait_timeouts,
             (unsigned long)g_health.rtc_alarm_errors,
             (unsigned long)g_health.dma_half_events,
             (unsigned long)g_health.dma_full_events,
              (unsigned long)g_health.sensor_errors,
               (unsigned long)spi_status.transfers,
               (unsigned long)spi_status.errors,
               (unsigned long)spi_status.recoveries,
               (unsigned long)spi_status.recovery_failures,
               (unsigned long)g_health.nrf24_reports,
               (unsigned long)g_health.nrf24_errors,
               (unsigned long)g_health.watchdog_faults,
              (unsigned int)g_watchdog_permit,
              (unsigned int)g_health.acquisition_stack_free_words,
              (unsigned int)g_health.processing_stack_free_words,
              (unsigned int)g_health.report_stack_free_words,
              (unsigned int)g_health.health_stack_free_words,
              (unsigned int)g_health.cycle_stack_free_words,
              (unsigned int)g_health.watchdog_stack_free_words,
              (unsigned long)g_health.stop_entries,
             (unsigned long)g_health.stop_wakeups,
             (unsigned long)osMessageQueueGetCount(g_cycle_request_queue),
             (unsigned long)osMessageQueueGetCount(g_sample_ready_queue),
             (unsigned long)osMessageQueueGetCount(g_result_queue));
    osDelay(MONITOR_HEALTH_PERIOD_MS);
  }
}

static void MonitoringTasks_WatchdogTask(void *argument)
{
  (void)argument;

  for (;;)
  {
    osDelay(1000U);
    if (g_health.cycles_started == 0U)
    {
      continue;
    }

    /*
     * 正常周期之间任务处于等待状态。尤其是 300 秒生产模式，不能把
     * “没有新采样”误判成任务停止；真正执行中的 ACQUIRE/PROCESS/REPORT
     * 状态才需要按心跳超时检查。
     */
    if (g_health.state == MONITOR_STATE_IDLE ||
        g_health.state == MONITOR_STATE_STOP ||
        g_health.state == MONITOR_STATE_SELF_TEST)
    {
      g_watchdog_permit = 1U;
      g_watchdog_last_progress_tick = HAL_GetTick();
      MonitoringHardwareWatchdog_Refresh();
      continue;
    }

    if (g_health.acquisition_heartbeat != g_watchdog_last_acquisition ||
        g_health.processing_heartbeat != g_watchdog_last_processing ||
        g_health.report_heartbeat != g_watchdog_last_report)
    {
      g_watchdog_last_acquisition = g_health.acquisition_heartbeat;
      g_watchdog_last_processing = g_health.processing_heartbeat;
      g_watchdog_last_report = g_health.report_heartbeat;
      g_watchdog_last_progress_tick = HAL_GetTick();
      g_watchdog_permit = 1U;
      MonitoringHardwareWatchdog_Refresh();
      continue;
    }

    if (g_health.state == MONITOR_STATE_FAULT ||
        (HAL_GetTick() - g_watchdog_last_progress_tick) > MONITOR_WATCHDOG_TIMEOUT_MS)
    {
      g_watchdog_permit = 0U;
      if (g_health.state != MONITOR_STATE_STOP)
      {
        g_health.watchdog_faults++;
        g_health.state = MONITOR_STATE_FAULT;
        MONITOR_LOG("[WATCHDOG] heartbeat stopped; software watchdog denied\r\n");
      }
      continue;
    }

    g_watchdog_permit = 1U;
    MonitoringHardwareWatchdog_Refresh();
  }
}

void MonitoringTasks_Create(void)
{
  monitor_sample_block_t *block;
  const monitoring_nrf24_bus_t nrf24_bus = {
    .transfer = MonitoringSpi_Transfer,
    .chip_select = MonitoringTasks_Nrf24ChipSelect,
    .chip_enable = MonitoringTasks_Nrf24ChipEnable,
    .ready = MONITOR_SPI_ENABLED != 0U ? 1U : 0U
  };
  const osMutexAttr_t log_mutex_attributes = {
    .name = "logMutex",
    .cb_mem = &g_log_mutex_cb,
    .cb_size = sizeof(g_log_mutex_cb)
  };

  /* USART1 保留为本地诊断/报告链路，NRF24 作为可选无线报告链路。 */
  MonitoringSpi_Init();
  MonitoringNrf24_Bind(&nrf24_bus);
  /* 无模块时返回 NOT_PRESENT，保持 UART 和采集链路继续运行。 */
  (void)MonitoringNrf24_Init();
  MonitoringHardwareWatchdog_Init();

  g_log_mutex = osMutexNew(&log_mutex_attributes);
  const osEventFlagsAttr_t cycle_event_attributes = {
    .name = "cycleEvent",
    .cb_mem = &g_cycle_event_cb,
    .cb_size = sizeof(g_cycle_event_cb)
  };
  g_monitor_cycle_event = osEventFlagsNew(&cycle_event_attributes);
  const osEventFlagsAttr_t control_event_attributes = {
    .name = "controlEvent",
    .cb_mem = &g_control_event_cb,
    .cb_size = sizeof(g_control_event_cb)
  };
  const osSemaphoreAttr_t report_done_sem_attributes = {
    .name = "reportDoneSem",
    .cb_mem = &g_report_done_sem_cb,
    .cb_size = sizeof(g_report_done_sem_cb)
  };
  g_monitor_control_event = osEventFlagsNew(&control_event_attributes);
  g_report_done_sem = osSemaphoreNew(1U, 0U, &report_done_sem_attributes);

  g_cycle_request_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_request_t),
    &g_cycle_request_queue_cb,
    g_cycle_request_queue_mem,
    sizeof(g_cycle_request_queue_mem),
    "cycleRequestQ");
  g_sample_free_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    &g_sample_free_queue_cb,
    g_sample_free_queue_mem,
    sizeof(g_sample_free_queue_mem),
    "sampleFreeQ");
  g_sample_ready_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    &g_sample_ready_queue_cb,
    g_sample_ready_queue_mem,
    sizeof(g_sample_ready_queue_mem),
    "sampleReadyQ");
  g_result_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_result_t),
    &g_result_queue_cb,
    g_result_queue_mem,
    sizeof(g_result_queue_mem),
    "resultQ");

  if (g_log_mutex == NULL || g_monitor_cycle_event == NULL ||
      g_monitor_control_event == NULL || g_cycle_request_queue == NULL ||
      g_sample_free_queue == NULL ||
      g_sample_ready_queue == NULL || g_result_queue == NULL ||
      g_report_done_sem == NULL)
  {
    UART_Log("[RTOS] task pipeline queue creation failed\r\n");
    g_health.state = MONITOR_STATE_FAULT;
    return;
  }

  for (uint32_t i = 0U; i < MONITOR_SAMPLE_POOL_SIZE; i++)
  {
    block = &g_sample_pool[i];
    if (osMessageQueuePut(g_sample_free_queue, &block, 0U, 0U) != osOK)
    {
      UART_Log("[RTOS] sample pool initialization failed, index=%lu\r\n",
               (unsigned long)i);
      g_health.state = MONITOR_STATE_FAULT;
      return;
    }
  }

  g_acquisition_task = MonitoringTasks_CreateThread(
    "acquisition_task", MonitoringTasks_AcquisitionTask, osPriorityHigh,
    &g_acquisition_task_cb, g_acquisition_task_stack,
    sizeof(g_acquisition_task_stack) / sizeof(g_acquisition_task_stack[0]));
  g_processing_task = MonitoringTasks_CreateThread(
    "processing_task", MonitoringTasks_ProcessingTask, osPriorityAboveNormal,
    &g_processing_task_cb, g_processing_task_stack,
    sizeof(g_processing_task_stack) / sizeof(g_processing_task_stack[0]));
  g_report_task = MonitoringTasks_CreateThread(
    "report_task", MonitoringTasks_ReportTask, osPriorityBelowNormal,
    &g_report_task_cb, g_report_task_stack,
    sizeof(g_report_task_stack) / sizeof(g_report_task_stack[0]));
  g_cycle_task = MonitoringTasks_CreateThread(
    "cycle_task", MonitoringTasks_RunCycleTask, osPriorityNormal,
    &g_cycle_task_cb, g_cycle_task_stack,
    sizeof(g_cycle_task_stack) / sizeof(g_cycle_task_stack[0]));
  g_health_task = MonitoringTasks_CreateThread(
    "health_task", MonitoringTasks_HealthTask, osPriorityLow,
    &g_health_task_cb, g_health_task_stack,
    sizeof(g_health_task_stack) / sizeof(g_health_task_stack[0]));
  g_watchdog_task = MonitoringTasks_CreateThread(
    "watchdog_task", MonitoringTasks_WatchdogTask, osPriorityLow,
    &g_watchdog_task_cb, g_watchdog_task_stack,
    sizeof(g_watchdog_task_stack) / sizeof(g_watchdog_task_stack[0]));

  if (g_acquisition_task == NULL || g_processing_task == NULL ||
      g_report_task == NULL || g_health_task == NULL || g_cycle_task == NULL ||
      g_watchdog_task == NULL)
  {
    UART_Log("[RTOS] task pipeline thread creation failed\r\n");
    g_health.state = MONITOR_STATE_FAULT;
    return;
  }

  MonitoringAlerts_Reset();
  MonitoringAcquisition_Init();
  g_watchdog_last_progress_tick = HAL_GetTick();
  g_watchdog_permit = 1U;
  UART_Log("[RTOS] pipeline ready: acquisition/processing/report/health/watchdog\r\n");
}

void MonitoringTasks_RunCycleTask(void *argument)
{
  monitor_cycle_request_t request;
  uint32_t event_flags;
  (void)argument;

  g_health.state = MONITOR_STATE_SELF_TEST;
  MONITOR_LOG("[RTOS] cycle manager start; waiting for RTC Alarm\r\n");
  MonitoringTasks_RearmRtcAlarm("boot");
  g_health.state = MONITOR_STATE_IDLE;

  for (;;)
  {
    event_flags = osEventFlagsWait(
      g_monitor_cycle_event,
      MONITOR_EVENT_RTC_ALARM,
      osFlagsWaitAny,
      MONITOR_CYCLE_WAIT_TIMEOUT_MS);

    if ((event_flags & MONITOR_EVENT_RTC_ALARM) == 0U)
    {
      g_health.rtc_wait_timeouts++;
      g_health.state = MONITOR_STATE_FAULT;
      MONITOR_LOG("[RTOS] RTC alarm wait timeout, recovering\r\n");
      /* 超时后主动重设闹钟，避免一次配置失败让链路永久停住。 */
      MonitoringTasks_RearmRtcAlarm("wait_timeout");
      g_health.state = MONITOR_STATE_IDLE;
      continue;
    }

    /* LED 心跳由 defaultTask 独占，周期任务只负责调度数据链路。 */
    request.cycle_id = ++g_health.cycles_started;
    request.timestamp_ticks = HAL_GetTick();
    g_health.state = MONITOR_STATE_ACQUIRE;

    if (osMessageQueuePut(g_cycle_request_queue, &request, 0U, 0U) != osOK)
    {
      g_health.acquire_drops++;
      g_health.state = MONITOR_STATE_FAULT;
      MONITOR_LOG("[RTOS] cycle=%lu request queue full\r\n",
                  (unsigned long)request.cycle_id);
      MonitoringTasks_RearmRtcAlarm("request_drop");
    }
    else
    {
      g_health.state = MONITOR_STATE_IDLE;
      if (g_report_done_sem != NULL)
      {
        if (osSemaphoreAcquire(g_report_done_sem,
                               MONITOR_REPORT_WAIT_TIMEOUT_MS) != osOK)
        {
          g_health.report_drops++;
          g_health.state = MONITOR_STATE_FAULT;
          MONITOR_LOG("[RTOS] report completion timeout; stop denied\r\n");
          MonitoringTasks_RearmRtcAlarm("report_timeout");
        }
        else
        {
          if (MonitoringTasks_EnterStop() == 0U)
          {
            MONITOR_LOG("[RTOS] stop gate denied; system remains running\r\n");
            MonitoringTasks_RearmRtcAlarm("stop_gate_denied");
          }
        }
      }
      else
      {
        g_health.report_drops++;
        g_health.state = MONITOR_STATE_FAULT;
        MONITOR_LOG("[RTOS] report semaphore unavailable; stop denied\r\n");
        MonitoringTasks_RearmRtcAlarm("report_sem_missing");
      }
    }
  }
}

void MonitoringTasks_OnMpuDataReady(void)
{
  /* 中断只发布事件；I2C 读 FIFO 仍由 acquisition_task 执行。 */
  if (g_monitor_control_event != NULL)
  {
    (void)osEventFlagsSet(g_monitor_control_event, MONITOR_EVENT_CAPTURE);
  }
}

void MonitoringTasks_OnAdcHalfComplete(void)
{
  g_health.dma_half_events++;
}

void MonitoringTasks_OnAdcComplete(void)
{
  g_health.dma_full_events++;
}
