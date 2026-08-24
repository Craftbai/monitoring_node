/* =============================================================================
 * monitoring_tasks.c —— FreeRTOS 调度链路
 *
 * 职责：周期状态机、采集/处理/上报任务流水线、Stop 门禁、健康监测、看门狗
 *
 * 分区结构：
 *   1/7: NRF24 上报（载荷编码 + 板级 CS/CE 回调）
 *   2/7: 固定静态资源（块池、队列、事件组、任务 CB/栈、信号量）
 *   3/7: 日志与健康（LogLock 宏、g_health、状态文本）
 *   4/7: 通用创建辅助（CreateQueue / CreateThread）
 *   5/7: Stop 门禁与采样块所有权（RearmRtcAlarm/EnterStop/ReturnSampleBlock）
 *   6/7: 任务实现（采集/处理/上报/健康/看门狗）
 *   7/7: 周期状态机（RunCycleTask）、调度装配（Create）、中断回调
 * ============================================================================= */

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

/* Stop 唤醒后由 main.c 提供系统时钟恢复函数。 */
extern void SystemClock_Config(void);

/* 内部常量 */
#define MONITOR_HEALTH_PERIOD_MS 10000U
#define MONITOR_WATCHDOG_TIMEOUT_MS 15000U

/* =============================================================================
 * 分区 1/7: NRF24 上报（载荷编码 + 板级 CS/CE 回调）
 * ============================================================================= */

/**
 * @brief  NRF24 片选控制回调
 * @param  active: 1=拉低 CSN（选中），0=拉高 CSN（释放）
 */
static void MonitoringTasks_Nrf24ChipSelect(uint8_t active)
{
  HAL_GPIO_WritePin(NRF24_CSN_GPIO_Port, NRF24_CSN_Pin,
                    active != 0U ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/**
 * @brief  NRF24 使能控制回调
 * @param  active: 1=拉高 CE（使能发送/接收），0=拉低 CE（待机）
 */
static void MonitoringTasks_Nrf24ChipEnable(uint8_t active)
{
  HAL_GPIO_WritePin(NRF24_CE_GPIO_Port, NRF24_CE_Pin,
                    active != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

#if MONITOR_NRF24_REPORT_ENABLED
/**
 * @brief  小端序编码 uint32_t 到载荷缓冲区
 * @param  buffer: 载荷缓冲区
 * @param  offset: 当前偏移量
 * @param  value: 要编码的 32 位值
 * @return 更新后的偏移量
 */
static uint8_t MonitoringTasks_PutU32(uint8_t *buffer, uint8_t offset,
                                      uint32_t value)
{
  buffer[offset++] = (uint8_t)(value & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 8U) & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 16U) & 0xFFU);
  buffer[offset++] = (uint8_t)((value >> 24U) & 0xFFU);
  return offset;
}

/**
 * @brief  构建 NRF24 上报载荷（固定 32 字节）
 * @param  result: 周期结果指针
 * @param  payload: 输出缓冲区（至少 MONITOR_NRF24_PAYLOAD_SIZE 字节）
 * @return 实际载荷长度（固定 32）
 * @note   当前使用固定长度管道，载荷补齐到 RX_PW_P0 配置的 32 字节，
 *         让接收端始终按同一个帧长度读取。
 */
static uint8_t MonitoringTasks_BuildNrf24Payload(
  const monitor_cycle_result_t *result, uint8_t *payload)
{
  uint8_t offset = 0U;

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

/* =============================================================================
 * 分区 2/7: 固定静态资源（块池、队列、事件组、任务 CB/栈、信号量）
 *
 * 数据流水线（单周期完整路径）：
 *   [RTC 闹钟中断]
 *        ↓ 发布 MONITOR_EVENT_RTC_ALARM
 *   [cycle_task] 生成 cycle_id
 *        ↓ 放入 cycle_request_queue
 *   [acquisition_task] 等待请求
 *        ↓ 从 sample_free_queue 取空块
 *   采集三通道（温度/振动/电流）
 *        ↓ 填充采样块，放入 sample_ready_queue
 *   [processing_task] 等待 ready 块
 *        ↓ 调用 MonitoringAlgorithm_Process（RMS/FFT/告警）
 *   生成 result，放入 result_queue，归还块到 sample_free_queue
 *        ↓
 *   [report_task] 等待 result
 *        ↓ UART 日志 + NRF24 无线发送
 *   释放 report_done_sem
 *        ↓
 *   [cycle_task] 等待信号量
 *        ↓ 调用 MonitoringTasks_EnterStop（门禁检查）
 *   进入 Stop 模式，等待下次 RTC 闹钟
 *
 * 任务协作关系（优先级 → 职责）：
 *   - acquisition_task (osPriorityHigh)
 *       职责：等待周期请求，从块池取空块，调用 MonitoringAcquisition_Capture
 *             完成 DS18B20/MPU6050/ACS712 三通道采集，放入 ready 队列
 *       阻塞点：cycle_request_queue、sample_free_queue（100ms 超时）
 *       错误处理：块池耗尽/采集超时/ready 队列满时记录 acquire_drops
 *
 *   - processing_task (osPriorityAboveNormal)
 *       职责：等待 ready 块，调用 MonitoringAlgorithm_Process 完成
 *             去直流、滤波、RMS、峰峰值、FFT 频带能量、告警评估，
 *             放入 result 队列，归还块到 free 队列
 *       阻塞点：sample_ready_queue、result_queue（100ms 超时）
 *       错误处理：result 队列满时记录 report_drops（处理已完成）
 *
 *   - report_task (osPriorityBelowNormal)
 *       职责：等待 result，通过 UART 输出详细日志（周期 ID/有效掩码/
 *             特征值/告警状态），若 NRF24 就绪则编码载荷并发送，
 *             最后释放 report_done_sem 通知 cycle_task
 *       阻塞点：result_queue、UART 传输、NRF24 发送
 *       错误处理：NRF24 发送失败时记录 nrf24_errors
 *
 *   - cycle_task (osPriorityNormal)
 *       职责：周期协调者，等待 RTC 闹钟事件 → 生成 cycle_id →
 *             发起采集请求 → 等待上报完成（信号量）→ 尝试进入 Stop
 *       阻塞点：osEventFlagsWait(RTC_ALARM, 超时=周期+10秒)、
 *               osSemaphoreAcquire(report_done, 超时=12秒)
 *       错误处理：RTC 等待超时/请求队列满/上报超时/Stop 门禁失败时，
 *                主动重设闹钟并继续下一周期
 *
 *   - health_task (osPriorityLow)
 *       职责：定期（10 秒）读取各任务栈水位、SPI 统计、队列深度，
 *             通过 UART 输出健康日志
 *       阻塞点：osDelay(MONITOR_HEALTH_PERIOD_MS)
 *
 *   - watchdog_task (osPriorityLow)
 *       职责：定期（1 秒）检查采集/处理/上报心跳是否更新，
 *             若 ACQUIRE/PROCESS/REPORT 状态下超过 15 秒无进展则
 *             拒绝看门狗许可（g_watchdog_permit = 0）
 *       阻塞点：osDelay(1000U)
 *       特殊逻辑：IDLE/STOP 状态不判超时（正常等待 RTC 闹钟）
 *
 * 队列用途与深度：
 *   - cycle_request_queue (深度 2)：RTC 闹钟触发频率低（5秒或300秒），
 *                                   深度 2 足够缓冲偶发的处理延迟
 *   - sample_free_queue (深度 2)：空闲块池，与 SAMPLE_POOL_SIZE 一一对应
 *   - sample_ready_queue (深度 2)：采集→处理传递，深度 2 允许采集提前
 *                                  一个周期（前一个还在处理时新周期已采集）
 *   - result_queue (深度 2)：处理→上报传递，深度 2 允许处理提前一个周期
 * ============================================================================= */

/* 固定块池：采集任务取得所有权，处理任务归还所有权。 */
static monitor_sample_block_t g_sample_pool[MONITOR_SAMPLE_POOL_SIZE];
static uint32_t g_sample_sequence = 0U;

/* 队列：cycle_request → acquisition → sample_ready → processing → result → report */
static osMessageQueueId_t g_cycle_request_queue;
static osMessageQueueId_t g_sample_free_queue;
static osMessageQueueId_t g_sample_ready_queue;
static osMessageQueueId_t g_result_queue;

/* 事件组：RTC 闹钟、采集控制 */
osEventFlagsId_t g_monitor_cycle_event;
osEventFlagsId_t g_monitor_control_event;

/* 队列和事件组的静态控制块 */
static StaticQueue_t g_cycle_request_queue_cb;
static StaticQueue_t g_sample_free_queue_cb;
static StaticQueue_t g_sample_ready_queue_cb;
static StaticQueue_t g_result_queue_cb;
static StaticEventGroup_t g_cycle_event_cb;
static StaticEventGroup_t g_control_event_cb;
static StaticSemaphore_t g_report_done_sem_cb;

/* 队列存储区 */
static uint8_t g_cycle_request_queue_mem[sizeof(monitor_cycle_request_t) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_sample_free_queue_mem[sizeof(monitor_sample_block_t *) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_sample_ready_queue_mem[sizeof(monitor_sample_block_t *) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));
static uint8_t g_result_queue_mem[sizeof(monitor_cycle_result_t) * MONITOR_QUEUE_DEPTH]
  __attribute__((aligned(4)));

/* 任务控制块 */
static StaticTask_t g_acquisition_task_cb;
static StaticTask_t g_processing_task_cb;
static StaticTask_t g_report_task_cb;
static StaticTask_t g_health_task_cb;
static StaticTask_t g_cycle_task_cb;
static StaticTask_t g_watchdog_task_cb;

/* 任务栈 */
static StackType_t g_acquisition_task_stack[256];
static StackType_t g_processing_task_stack[256];
static StackType_t g_report_task_stack[192];
static StackType_t g_health_task_stack[192];
static StackType_t g_cycle_task_stack[256];
static StackType_t g_watchdog_task_stack[160];

/* 任务句柄 */
static osThreadId_t g_acquisition_task;
static osThreadId_t g_processing_task;
static osThreadId_t g_report_task;
static osThreadId_t g_health_task;
static osThreadId_t g_cycle_task;
static osThreadId_t g_watchdog_task;

/* 日志互斥量和上报完成信号量 */
static osMutexId_t g_log_mutex;
static osSemaphoreId_t g_report_done_sem;
static StaticSemaphore_t g_log_mutex_cb;

/* 看门狗许可与心跳 */
static volatile uint8_t g_watchdog_permit;
static uint32_t g_watchdog_last_acquisition;
static uint32_t g_watchdog_last_processing;
static uint32_t g_watchdog_last_report;
static uint32_t g_watchdog_last_progress_tick;

/* =============================================================================
 * 分区 3/7: 日志与健康（LogLock 宏、g_health、状态文本）
 * ============================================================================= */

/**
 * @brief  日志互斥锁获取
 * @return 1=成功获取，0=失败或内核未运行
 */
static uint8_t MonitoringTasks_LogLock(void)
{
  if (g_log_mutex != NULL && osKernelGetState() == osKernelRunning)
  {
    return (osMutexAcquire(g_log_mutex, 100U) == osOK) ? 1U : 0U;
  }

  return 0U;
}

/**
 * @brief  日志互斥锁释放
 */
static void MonitoringTasks_LogUnlock(void)
{
  if (g_log_mutex != NULL && osKernelGetState() == osKernelRunning)
  {
    (void)osMutexRelease(g_log_mutex);
  }
}

/**
 * @brief  带互斥保护的日志宏
 * @note   只在任务上下文使用，中断中直接调用 UART_Log
 */
#define MONITOR_LOG(...) do { \
  uint8_t monitor_log_locked = MonitoringTasks_LogLock(); \
  UART_Log(__VA_ARGS__); \
  if (monitor_log_locked != 0U) { MonitoringTasks_LogUnlock(); } \
} while (0)

/* 健康统计全局实例 */
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

/**
 * @brief  状态机状态转字符串
 * @param  state: 状态枚举值
 * @return 状态文本指针（常量字符串）
 */
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

/* =============================================================================
 * 分区 4/7: 通用创建辅助（CreateQueue / CreateThread）
 * ============================================================================= */

/**
 * @brief  创建静态消息队列
 * @param  count: 队列深度
 * @param  item_size: 单个元素字节数
 * @param  control_block: 静态控制块指针
 * @param  storage: 队列存储区指针
 * @param  storage_size: 存储区字节数
 * @param  name: 队列名称（用于调试）
 * @return 队列句柄，失败返回 NULL
 */
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

/**
 * @brief  创建静态任务线程
 * @param  name: 任务名称（用于调试）
 * @param  function: 任务入口函数
 * @param  priority: 任务优先级
 * @param  control_block: 静态控制块指针
 * @param  stack: 任务栈指针
 * @param  stack_words: 栈大小（单位：StackType_t）
 * @return 任务句柄，失败返回 NULL
 */
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

/* =============================================================================
 * 分区 5/7: Stop 门禁与采样块所有权（RearmRtcAlarm/EnterStop/ReturnSampleBlock）
 * ============================================================================= */

/**
 * @brief  重设 RTC 闹钟（集中处理，避免异常路径漏掉下一次 RTC 事件）
 * @param  reason: 重设原因字符串（用于日志）
 */
static void MonitoringTasks_RearmRtcAlarm(const char *reason)
{
  if (RTC_SetNextAlarm(MONITOR_CYCLE_INTERVAL_SEC) != HAL_OK)
  {
    g_health.rtc_alarm_errors++;
    MONITOR_LOG("[RTOS] RTC alarm rearm failed, reason=%s\r\n", reason);
  }
}

/**
 * @brief  进入 Stop 模式的门禁与执行
 * @return 1=成功进入并唤醒，0=门禁失败或唤醒异常
 * @note   门禁条件：
 *         - 非 FAULT 状态且软件看门狗许可
 *         - 硬件 IWDG 未启用（F103 Stop 期间 IWDG 可能继续计时）
 *         - 所有队列已清空、块池完全归还
 *         - 采集已停止、DMA/传感器中断已清理
 *         - RTC 闹钟已成功设置
 */
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

/**
 * @brief  归还采样块到空闲队列（集中处理所有权流转失败）
 * @param  block: 指向块指针的指针（归还后会清零）
 * @return 1=成功归还，0=失败
 * @note   采集块只允许沿着 free -> ready -> free 的方向流转。
 *         这里集中处理归还失败，避免异常时块从池中静默消失，
 *         导致后续周期一直拿不到块。
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

/* =============================================================================
 * 分区 6/7: 任务实现（采集/处理/上报/健康/看门狗）
 * ============================================================================= */

/**
 * @brief  采集任务（高优先级）
 * @param  argument: 未使用
 * @note   职责：从 cycle_request_queue 取周期请求，从块池取空块，
 *         调用 MonitoringAcquisition_Capture 完成温度/振动/电流采集，
 *         将填充后的块放入 sample_ready_queue。
 * @note   阻塞点：等待 cycle_request、等待空闲块、放入 ready 队列
 * @note   错误路径：空闲块耗尽、采集超时、ready 队列满时记录丢块
 */
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

/**
 * @brief  处理任务（中高优先级）
 * @param  argument: 未使用
 * @note   职责：从 sample_ready_queue 取采样块，调用 MonitoringAlgorithm_Process
 *         完成去直流、滤波、RMS、峰峰值、FFT 频带能量、告警评估，
 *         将结果放入 result_queue，归还块到空闲队列。
 * @note   阻塞点：等待 ready 块、放入 result 队列
 * @note   错误路径：result 队列满时记录 report_drops
 */
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

/**
 * @brief  上报任务（中低优先级）
 * @param  argument: 未使用
 * @note   职责：从 result_queue 取周期结果，通过 UART 输出详细日志，
 *         若 NRF24 就绪则编码载荷并发送，最后释放 report_done_sem 通知周期任务。
 * @note   阻塞点：等待 result、UART 传输、NRF24 发送
 * @note   错误路径：NRF24 发送失败时记录 nrf24_errors
 */
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

/**
 * @brief  健康监测任务（低优先级）
 * @param  argument: 未使用
 * @note   职责：定期（10 秒）读取各任务栈水位、SPI 统计、队列深度，
 *         通过 UART 输出健康日志。
 * @note   阻塞点：osDelay(MONITOR_HEALTH_PERIOD_MS)
 */
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

/**
 * @brief  软件看门狗任务（低优先级）
 * @param  argument: 未使用
 * @note   职责：定期（1 秒）检查采集/处理/上报心跳是否更新，
 *         若 ACQUIRE/PROCESS/REPORT 状态下超过 15 秒无进展则拒绝看门狗许可。
 * @note   周期任务处于 IDLE/STOP 时不判超时（正常等待 RTC 闹钟）。
 * @note   阻塞点：osDelay(1000U)
 */
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
     * "没有新采样"误判成任务停止；真正执行中的 ACQUIRE/PROCESS/REPORT
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

/* =============================================================================
 * 分区 7/7: 周期状态机（RunCycleTask）、调度装配（Create）、中断回调
 * ============================================================================= */

/**
 * @brief  周期协调任务（中优先级）
 * @param  argument: 未使用
 * @note   职责：等待 RTC 闹钟事件 → 生成 cycle_id → 发起采集请求 →
 *         等待上报完成（信号量）→ 尝试进入 Stop 模式。
 * @note   阻塞点：osEventFlagsWait(RTC_ALARM)、osSemaphoreAcquire(report_done)
 * @note   错误路径：RTC 等待超时、请求队列满、上报超时、Stop 门禁失败时，
 *         主动重设闹钟并继续下一周期。
 */
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

/**
 * @brief  创建固定队列、块池和应用任务（调度装配入口）
 * @note   在 freertos.c 的 MX_FREERTOS_Init 中调用
 * @note   初始化顺序：SPI/NRF24 → 看门狗 → 日志锁 → 事件组 → 队列 →
 *         块池预填充 → 任务创建 → 告警/采集初始化
 */
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

  /* ===== 第 1 步：初始化底层驱动（SPI/NRF24/IWDG） ===== */
  /* 必须在任务创建前完成，避免任务运行时驱动未就绪。
   * - MonitoringSpi_Init(): 初始化 SPI2 状态统计
   * - MonitoringNrf24_Bind(): 绑定 SPI 传输和 CSN/CE 控制回调
   * - MonitoringNrf24_Init(): 配置 NRF24 寄存器（无模块时返回 NOT_PRESENT）
   * - MonitoringHardwareWatchdog_Init(): 配置 IWDG（当前禁用，仅软件看门狗）
   */
  MonitoringSpi_Init();
  MonitoringNrf24_Bind(&nrf24_bus);
  (void)MonitoringNrf24_Init();  /* 无模块时不影响 UART 主链路 */
  MonitoringHardwareWatchdog_Init();

  /* ===== 第 2 步：创建同步资源（互斥量、事件组、信号量） ===== */
  /* 这些资源必须在任务创建前就绪，否则任务运行时访问 NULL 句柄会崩溃。
   *
   * g_log_mutex: 日志互斥量
   *   - 保护 UART_Log 的多任务并发调用
   *   - MONITOR_LOG 宏会先获取锁，再调用 UART_Log，最后释放锁
   */
  g_log_mutex = osMutexNew(&log_mutex_attributes);

  /*
   * g_monitor_cycle_event: 周期事件组
   *   - RTC 闹钟中断发布 MONITOR_EVENT_RTC_ALARM 标志
   *   - cycle_task 等待此事件后生成新周期请求
   */
  const osEventFlagsAttr_t cycle_event_attributes = {
    .name = "cycleEvent",
    .cb_mem = &g_cycle_event_cb,
    .cb_size = sizeof(g_cycle_event_cb)
  };
  g_monitor_cycle_event = osEventFlagsNew(&cycle_event_attributes);

  /*
   * g_monitor_control_event: 采集控制事件组
   *   - MPU6050 数据就绪中断（PE2/EXTI2）发布 MONITOR_EVENT_CAPTURE 标志
   *   - acquisition_task 等待此事件后读取 FIFO（避免在中断中访问 I2C）
   */
  const osEventFlagsAttr_t control_event_attributes = {
    .name = "controlEvent",
    .cb_mem = &g_control_event_cb,
    .cb_size = sizeof(g_control_event_cb)
  };
  g_monitor_control_event = osEventFlagsNew(&control_event_attributes);

  /*
   * g_report_done_sem: 上报完成信号量（二值信号量，初始值 0）
   *   - report_task 完成 UART/NRF24 上报后释放信号量
   *   - cycle_task 等待信号量（12 秒超时），收到后尝试进入 Stop 模式
   *   - 这个握手机制确保进 Stop 前本周期结果已上报完成
   */
  const osSemaphoreAttr_t report_done_sem_attributes = {
    .name = "reportDoneSem",
    .cb_mem = &g_report_done_sem_cb,
    .cb_size = sizeof(g_report_done_sem_cb)
  };
  g_report_done_sem = osSemaphoreNew(1U, 0U, &report_done_sem_attributes);

  /* ===== 第 3 步：创建消息队列 ===== */
  /* 队列必须在任务创建前就绪，任务启动后会立即调用 osMessageQueueGet。
   * 所有队列使用静态分配（预分配控制块和存储区），避免运行期依赖堆。
   *
   * g_cycle_request_queue (深度 2, 元素 sizeof(monitor_cycle_request_t))
   *   - 生产者: cycle_task（RTC 闹钟触发后放入）
   *   - 消费者: acquisition_task（阻塞等待）
   *   - 用途: 传递周期 ID 和时间戳，触发新一轮采集
   */
  g_cycle_request_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_request_t),
    &g_cycle_request_queue_cb,
    g_cycle_request_queue_mem,
    sizeof(g_cycle_request_queue_mem),
    "cycleRequestQ");

  /*
   * g_sample_free_queue (深度 2, 元素 sizeof(monitor_sample_block_t *))
   *   - 初始化时: 将块池中所有空块指针预填充到此队列
   *   - 生产者: processing_task（处理完成后归还）
   *   - 消费者: acquisition_task（采集前取块，100ms 超时）
   *   - 用途: 管理采样块所有权，避免并发覆盖
   */
  g_sample_free_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    &g_sample_free_queue_cb,
    g_sample_free_queue_mem,
    sizeof(g_sample_free_queue_mem),
    "sampleFreeQ");

  /*
   * g_sample_ready_queue (深度 2, 元素 sizeof(monitor_sample_block_t *))
   *   - 生产者: acquisition_task（采集完成后放入）
   *   - 消费者: processing_task（阻塞等待）
   *   - 用途: 传递已填充的采样块指针
   *   - 深度 2 允许采集提前一个周期（前一个还在处理时新周期已采集）
   */
  g_sample_ready_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    &g_sample_ready_queue_cb,
    g_sample_ready_queue_mem,
    sizeof(g_sample_ready_queue_mem),
    "sampleReadyQ");

  /*
   * g_result_queue (深度 2, 元素 sizeof(monitor_cycle_result_t))
   *   - 生产者: processing_task（算法完成后放入，100ms 超时）
   *   - 消费者: report_task（阻塞等待）
   *   - 用途: 传递周期结果（特征值、告警状态、处理耗时）
   *   - 注意: 传递的是结构体本身（值拷贝），不是指针
   */
  g_result_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_result_t),
    &g_result_queue_cb,
    g_result_queue_mem,
    sizeof(g_result_queue_mem),
    "resultQ");

  /* 失败检查：任何一个同步资源或队列创建失败都进入 FAULT 状态 */
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

  /* ===== 第 4 步：初始化块池 ===== */
  /* 将所有空块预先放入 sample_free_queue，供 acquisition_task 取用。
   * 块池与空闲队列必须一一对应（SAMPLE_POOL_SIZE == QUEUE_DEPTH），
   * 否则可能出现块永久丢失。
   */
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

  /* ===== 第 5 步：创建任务（按依赖顺序） ===== */
  /* 任务优先级决定抢占关系：
   *   高 > 中高 > 中 > 中低 > 低
   * 所有任务使用静态分配（预分配控制块和栈），避免运行期依赖堆。
   *
   * acquisition_task (osPriorityHigh, 栈 256 words = 1024 字节)
   *   - 最高优先级，确保采集窗口不被其他任务打断
   *   - 职责: 等待周期请求 → 取空块 → 采集三通道 → 放 ready 队列
   *   - 栈需求: I2C 驱动 + FIFO 读取 + DMA 控制
   */
  g_acquisition_task = MonitoringTasks_CreateThread(
    "acquisition_task", MonitoringTasks_AcquisitionTask, osPriorityHigh,
    &g_acquisition_task_cb, g_acquisition_task_stack,
    sizeof(g_acquisition_task_stack) / sizeof(g_acquisition_task_stack[0]));

  /*
   * processing_task (osPriorityAboveNormal, 栈 256 words = 1024 字节)
   *   - 中高优先级，低于采集但高于上报和周期协调
   *   - 职责: 等待 ready 块 → CMSIS-DSP 算法 → 放 result 队列 → 归还块
   *   - 栈需求: Q15 FFT 工作区（已静态分配到全局）+ 函数调用栈
   */
  g_processing_task = MonitoringTasks_CreateThread(
    "processing_task", MonitoringTasks_ProcessingTask, osPriorityAboveNormal,
    &g_processing_task_cb, g_processing_task_stack,
    sizeof(g_processing_task_stack) / sizeof(g_processing_task_stack[0]));

  /*
   * report_task (osPriorityBelowNormal, 栈 192 words = 768 字节)
   *   - 中低优先级，可以被采集/处理打断
   *   - 职责: 等待 result → UART 日志 → NRF24 发送 → 释放 done 信号量
   *   - 栈需求: UART 格式化缓冲区 + NRF24 载荷编码
   */
  g_report_task = MonitoringTasks_CreateThread(
    "report_task", MonitoringTasks_ReportTask, osPriorityBelowNormal,
    &g_report_task_cb, g_report_task_stack,
    sizeof(g_report_task_stack) / sizeof(g_report_task_stack[0]));

  /*
   * cycle_task (osPriorityNormal, 栈 256 words = 1024 字节)
   *   - 中优先级，作为周期协调者，平衡采集和上报
   *   - 职责: 等 RTC 闹钟 → 生成 cycle_id → 发请求 → 等 done → 进 Stop
   *   - 栈需求: Stop 门禁检查 + 外设去初始化/重初始化
   */
  g_cycle_task = MonitoringTasks_CreateThread(
    "cycle_task", MonitoringTasks_RunCycleTask, osPriorityNormal,
    &g_cycle_task_cb, g_cycle_task_stack,
    sizeof(g_cycle_task_stack) / sizeof(g_cycle_task_stack[0]));

  /*
   * health_task (osPriorityLow, 栈 192 words = 768 字节)
   *   - 低优先级，只做监测，不影响主流程
   *   - 职责: 定期（10 秒）输出栈水位、SPI 统计、队列深度
   *   - 栈需求: uxTaskGetStackHighWaterMark 调用 + 日志格式化
   */
  g_health_task = MonitoringTasks_CreateThread(
    "health_task", MonitoringTasks_HealthTask, osPriorityLow,
    &g_health_task_cb, g_health_task_stack,
    sizeof(g_health_task_stack) / sizeof(g_health_task_stack[0]));

  /*
   * watchdog_task (osPriorityLow, 栈 160 words = 640 字节)
   *   - 低优先级，只做监测，不影响主流程
   *   - 职责: 定期（1 秒）检查任务心跳，超时拒绝看门狗许可
   *   - 栈需求: 最小（只有简单的心跳对比和 HAL_GetTick 调用）
   */
  g_watchdog_task = MonitoringTasks_CreateThread(
    "watchdog_task", MonitoringTasks_WatchdogTask, osPriorityLow,
    &g_watchdog_task_cb, g_watchdog_task_stack,
    sizeof(g_watchdog_task_stack) / sizeof(g_watchdog_task_stack[0]));

  /* 失败检查：任何一个任务创建失败都进入 FAULT 状态 */
  if (g_acquisition_task == NULL || g_processing_task == NULL ||
      g_report_task == NULL || g_health_task == NULL || g_cycle_task == NULL ||
      g_watchdog_task == NULL)
  {
    UART_Log("[RTOS] task pipeline thread creation failed\r\n");
    g_health.state = MONITOR_STATE_FAULT;
    return;
  }

  /* ===== 第 6 步：初始化应用层 ===== */
  /* 任务已创建但尚未运行（调度器在 main.c 的 osKernelStart 后才启动），
   * 此时可以安全地初始化应用层状态。
   *
   * MonitoringAlerts_Reset(): 告警状态机复位（所有通道进入 NORMAL 状态）
   * MonitoringAcquisition_Init(): ADC 校准、MPU6050 初始化
   */
  MonitoringAlerts_Reset();
  MonitoringAcquisition_Init();

  /* 看门狗初始许可：允许任务启动后立即开始喂狗 */
  g_watchdog_last_progress_tick = HAL_GetTick();
  g_watchdog_permit = 1U;

  UART_Log("[RTOS] pipeline ready: acquisition/processing/report/health/watchdog\r\n");
}

/**
 * @brief  MPU6050 数据就绪中断回调（PE2/EXTI2）
 * @note   中断只发布事件；I2C 读 FIFO 仍由 acquisition_task 执行。
 */
void MonitoringTasks_OnMpuDataReady(void)
{
  if (g_monitor_control_event != NULL)
  {
    (void)osEventFlagsSet(g_monitor_control_event, MONITOR_EVENT_CAPTURE);
  }
}

/**
 * @brief  ADC DMA 半传输中断回调
 * @note   仅更新计数，不操作缓冲区（任务中 memcpy）
 */
void MonitoringTasks_OnAdcHalfComplete(void)
{
  g_health.dma_half_events++;
}

/**
 * @brief  ADC DMA 全传输中断回调
 * @note   仅更新计数，不操作缓冲区（任务中 memcpy）
 */
void MonitoringTasks_OnAdcComplete(void)
{
  g_health.dma_full_events++;
}
