/* =============================================================================
 * monitoring_tasks.c —— FreeRTOS 调度链路
 *
 * 职责：周期状态机、采集/处理/上报任务流水线、Stop 门禁、健康监测、看门狗
 *
 * 分区结构：
 *   1/7: NRF24 上报（载荷编码 + 板级 CS/CE 回调）
 *   2/7: 采样块池（静态）；任务/队列/事件组/信号量句柄（动态）
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
#include "monitoring_bus.h"
#include "monitoring_drivers.h"
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
void MonitoringTasks_Nrf24ChipSelect(uint8_t active)
{
  HAL_GPIO_WritePin(NRF24_CSN_GPIO_Port, NRF24_CSN_Pin,
                    active != 0U ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/**
 * @brief  NRF24 使能控制回调
 * @param  active: 1=拉高 CE（使能发送/接收），0=拉低 CE（待机）
 */
void MonitoringTasks_Nrf24ChipEnable(uint8_t active)
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
 * 分区 2/7: 采样块池（静态）；任务/队列/事件组/信号量句柄（动态）
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

/* 队列/事件组/信号量/互斥量均动态分配：控制块与存储区由 osXXXNew 从 FreeRTOS 堆申请 */

/* 任务句柄（动态分配：TCB 与栈由 osThreadNew 从 FreeRTOS 堆申请） */
static osThreadId_t g_acquisition_task;
static osThreadId_t g_processing_task;
static osThreadId_t g_report_task;
static osThreadId_t g_health_task;
static osThreadId_t g_cycle_task;
static osThreadId_t g_watchdog_task;

/* 日志互斥量和上报完成信号量 */
static osMutexId_t g_log_mutex;
static osSemaphoreId_t g_report_done_sem;

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
 * @brief  创建动态消息队列
 * @param  count: 队列深度
 * @param  item_size: 单个元素字节数
 * @param  name: 队列名称（用于调试）
 * @return 队列句柄，失败返回 NULL
 * @note   不传 cb_mem/mq_mem，控制块与存储区由 osMessageQueueNew 从堆分配。
 */
static osMessageQueueId_t MonitoringTasks_CreateQueue(uint32_t count,
                                                       uint32_t item_size,
                                                       const char *name)
{
  const osMessageQueueAttr_t attributes = {
    .name = name
  };

  return osMessageQueueNew(count, item_size, &attributes);
}

/**
 * @brief  创建动态任务线程
 * @param  name: 任务名称（用于调试）
 * @param  function: 任务入口函数
 * @param  priority: 任务优先级
 * @param  stack_bytes: 栈大小（单位：字节）
 * @return 任务句柄，失败返回 NULL
 * @note   不传 cb_mem/stack_mem，TCB 与栈由 osThreadNew 从 FreeRTOS 堆分配。
 *         osThreadNew 内部按 sizeof(StackType_t) 将字节数换算为栈字。
 */
static osThreadId_t MonitoringTasks_CreateThread(const char *name,
                                                  osThreadFunc_t function,
                                                  osPriority_t priority,
                                                  uint32_t stack_bytes)
{
  const osThreadAttr_t attributes = {
    .name = name,
    .stack_size = stack_bytes,
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
  /* NRF24 已在 MonitoringDrivers_Resume() 中重新初始化 */
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
    /* ===== 步骤 1：等待周期请求 ===== */
    /* 由 RunCycleTask 在 RTC 闹钟唤醒后发送周期请求 */
    if (osMessageQueueGet(g_cycle_request_queue, &request, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    /* ===== 步骤 2：从空闲队列取块 ===== */
    /* 采样块池大小固定（MONITOR_SAMPLE_POOL_SIZE = 2），
     * 如果处理任务卡死或队列满，这里会超时（100ms） */
    g_health.state = MONITOR_STATE_ACQUIRE;
    if (osMessageQueueGet(g_sample_free_queue, &block, NULL, 100U) != osOK)
    {
      /* 空闲块耗尽：处理任务可能卡死或队列泄漏 */
      g_health.acquire_drops++;
      g_health.state = MONITOR_STATE_FAULT;
      continue;
    }

    /* ===== 步骤 3：初始化采样块元数据 ===== */
    memset(block, 0, sizeof(*block));
    block->cycle_id = request.cycle_id;           /* 周期 ID（全局递增） */
    block->sequence = ++g_sample_sequence;        /* 采样序列号（全局递增） */
    block->timestamp_ticks = request.timestamp_ticks; /* 周期开始时间戳 */
    block->channel_mask = MONITOR_VALID_TEMPERATURE |
                          MONITOR_VALID_VIBRATION |
                          MONITOR_VALID_CURRENT;  /* 标记所有通道待采集 */

    /* ===== 步骤 4：执行实际采集（阻塞最长 2.5 秒） ===== */
    /* MonitoringAcquisition_Capture 轮询三个通道：
     *   - 温度：DS18B20（约 750ms 转换 + 轮询）
     *   - 振动：MPU6050（1.28 秒采集 1280 个样本）
     *   - 电流：ADC DMA（1.024 秒采集 1024 个样本）
     * 返回后 block->flags 指示各通道的有效性、超时、溢出等状态 */
    MonitoringAcquisition_Capture(block);

    /* 检查传感器错误（所有通道都无效） */
    if ((block->flags & MONITOR_SAMPLE_FLAG_NO_SENSOR) != 0U)
    {
      g_health.sensor_errors++;
    }
    g_health.acquisition_heartbeat++;

    /* ===== 步骤 5：将填充后的块放入 ready 队列 ===== */
    /* 交付给 ProcessingTask 进行算法处理（去直流、RMS、FFT 等） */
    if (osMessageQueuePut(g_sample_ready_queue, &block, 0U, 100U) != osOK)
    {
      /* ready 队列满：处理任务可能卡死 */
      g_health.acquire_drops++;
      MonitoringTasks_ReturnSampleBlock(&block);  /* 归还块到 free 队列 */
      g_health.state = MONITOR_STATE_FAULT;
      continue;
    }

    /* ===== 完成：等待下一个周期请求 ===== */
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
    /* ===== 步骤 1：等待采样块 ===== */
    /* 由 AcquisitionTask 填充后放入 ready 队列 */
    if (osMessageQueueGet(g_sample_ready_queue, &block, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    /* ===== 步骤 2：初始化结果结构 ===== */
    g_health.state = MONITOR_STATE_PROCESS;
    start_ticks = HAL_GetTick();  /* 记录处理开始时间 */
    memset(&result, 0, sizeof(result));
    result.cycle_id = block->cycle_id;            /* 继承周期 ID */
    result.config_version = MONITOR_CONFIG_VERSION; /* 记录配置版本 */
    result.state = MONITOR_STATE_EVALUATE;        /* 状态标记为评估中 */

    /* ===== 步骤 3：执行算法处理 ===== */
    /* MonitoringAlgorithm_Process 执行：
     *   1. 温度：保存原始值
     *   2. 振动（三轴）：
     *      - 去直流（减去均值）
     *      - 转换为 Q15 定点数
     *      - 加窗（汉宁窗）
     *      - 1024 点 FFT（CMSIS-DSP）
     *      - 计算 RMS、峰峰值
     *      - 提取频带能量（0-10Hz, 10-40Hz, 40-150Hz, 150-400Hz）
     *   3. 电流：
     *      - 去直流（减去零点偏置）
     *      - 转换为 Q15
     *      - 计算 RMS
     *   4. 告警评估：
     *      - 比较各通道值与阈值
     *      - 更新连续超限/恢复计数
     *      - 确定告警状态（PENDING/ACTIVE/RECOVERING/NORMAL）
     *
     * 算法时间复杂度：O(N log N)，主要开销在 FFT（约 50-100ms） */
    MonitoringAlgorithm_Process(block, &result);
    result.processing_time_ms = HAL_GetTick() - start_ticks;
    g_health.processing_heartbeat++;

    /* ===== 步骤 4：将结果放入 result 队列 ===== */
    /* 交付给 ReportTask 进行无线上报 */
    if (osMessageQueuePut(g_result_queue, &result, 0U, 100U) != osOK)
    {
      /* 结果队列满：上报任务可能卡死（处理已完成，归类为上报侧丢弃） */
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
    /* ===== 步骤 1：等待处理结果 ===== */
    /* 由 ProcessingTask 完成算法处理后放入 result 队列 */
    if (osMessageQueueGet(g_result_queue, &result, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    /* ===== 步骤 2：准备上报数据 ===== */
    g_health.state = MONITOR_STATE_REPORT;
    /* 计算温度的绝对值（用于格式化输出，避免负数显示问题） */
    absolute_temperature = result.temperature_centi < 0
                             ? -(int32_t)result.temperature_centi
                             : result.temperature_centi;

    /* ===== 步骤 3：通过 UART 输出详细日志 ===== */
    /* 日志包含：
     *   - cycle_id：周期 ID
     *   - state：状态机状态
     *   - valid_mask：有效通道掩码（哪些通道成功采集）
     *   - error_flags：错误标志（采集/处理错误）
     *   - sample_flags：采样标志（超时、溢出等）
     *   - temp：温度（℃，精度 0.01）
     *   - current：电流（mA）
     *   - vib：振动三轴 RMS（mg）
     *   - alert_mask：告警掩码（哪些通道触发告警）
     *   - alert_state：告警状态（NORMAL/PENDING/ACTIVE/RECOVERING）
     *   - count：各通道的连续超限/恢复计数
     *   - process_ms：处理时间（ms） */
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

    /* ===== 步骤 4：通过 NRF24L01 无线上报（可选） ===== */
#if MONITOR_NRF24_REPORT_ENABLED
     if (MonitoringNrf24_IsReady() != 0U)
     {
       /* 将结果编码为紧凑的二进制载荷（约 30-40 字节） */
       nrf24_payload_length = MonitoringTasks_BuildNrf24Payload(
         &result, nrf24_payload);

       /* 发送到接收节点（最多 3 次重传） */
       nrf24_status = MonitoringNrf24_SendPayload(
         nrf24_payload, nrf24_payload_length);

       if (nrf24_status != MONITOR_NRF24_OK)
       {
         /* 发送失败：可能是无应答、CRC 错误、或超时 */
         g_health.nrf24_errors++;
       }
       else
       {
         g_health.nrf24_reports++;
       }
     }
#endif

    /* ===== 步骤 5：通知周期任务上报完成 ===== */
    /* 释放信号量，允许 RunCycleTask 进入 Stop 模式 */
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
  monitoring_bus_status_t spi_status;
  (void)argument;

  for (;;)
  {
    /* ===== 步骤 1：收集 SPI 总线统计 ===== */
    /* SPI 用于 NRF24L01 通信，统计包含：传输次数、错误次数、恢复次数 */
    MonitoringSpi_GetStatus(&spi_status);

    /* ===== 步骤 2：读取所有任务的栈水位 ===== */
    /* uxTaskGetStackHighWaterMark 返回栈的最小剩余空间（单位：字，4 字节）
     * 值越小表示栈使用越接近上限，值为 0 表示栈溢出风险 */
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

    /* ===== 步骤 3：输出健康日志 ===== */
    /* 日志包含：
     *   - state：当前状态机状态
     *   - cycles/ready/processed/reports：各阶段计数器
     *   - drops：各阶段丢块计数
     *   - rtc：RTC 超时和错误
     *   - dma：DMA 事件计数
     *   - sensor：传感器错误
     *   - spi：SPI 统计
     *   - nrf：NRF24 上报和错误
     *   - wd：看门狗故障和喂狗许可
     *   - stack：各任务栈剩余空间（字）
     *   - stop：Stop 进入和唤醒次数
     *   - q：各队列当前深度 */
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

    /* ===== 步骤 4：休眠 10 秒 ===== */
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
    /* ===== 每秒检查一次 ===== */
    osDelay(1000U);

    /* ===== 启动前跳过检查 ===== */
    /* 系统启动阶段（cycles_started == 0），还未进入正常周期，跳过看门狗检查 */
    if (g_health.cycles_started == 0U)
    {
      continue;
    }

    /* ===== 场景 1：空闲/Stop/自检状态 - 允许喂狗 ===== */
    /* 在长周期模式（如 300 秒）中，任务大部分时间处于 IDLE 或 STOP 状态，
     * 这是正常的，不应该判定为超时。只有在 ACQUIRE/PROCESS/REPORT 执行状态
     * 下才需要检查心跳是否更新。 */
    if (g_health.state == MONITOR_STATE_IDLE ||
        g_health.state == MONITOR_STATE_STOP ||
        g_health.state == MONITOR_STATE_SELF_TEST)
    {
      g_watchdog_permit = 1U;
      g_watchdog_last_progress_tick = HAL_GetTick();
      MonitoringHardwareWatchdog_Refresh();  /* 喂狗 */
      continue;
    }

    /* ===== 场景 2：心跳有更新 - 允许喂狗 ===== */
    /* 检查采集/处理/上报任务的心跳计数器是否有更新，
     * 任一任务有进展即视为系统正常运行 */
    if (g_health.acquisition_heartbeat != g_watchdog_last_acquisition ||
        g_health.processing_heartbeat != g_watchdog_last_processing ||
        g_health.report_heartbeat != g_watchdog_last_report)
    {
      /* 记录当前心跳快照 */
      g_watchdog_last_acquisition = g_health.acquisition_heartbeat;
      g_watchdog_last_processing = g_health.processing_heartbeat;
      g_watchdog_last_report = g_health.report_heartbeat;
      g_watchdog_last_progress_tick = HAL_GetTick();
      g_watchdog_permit = 1U;
      MonitoringHardwareWatchdog_Refresh();  /* 喂狗 */
      continue;
    }

    /* ===== 场景 3：FAULT 状态或心跳超时 - 拒绝喂狗 ===== */
    /* 如果系统处于 FAULT 状态，或者心跳在 MONITOR_WATCHDOG_TIMEOUT_MS（10秒）
     * 内没有更新，则拒绝喂狗，最终触发硬件看门狗复位 */
    if (g_health.state == MONITOR_STATE_FAULT ||
        (HAL_GetTick() - g_watchdog_last_progress_tick) > MONITOR_WATCHDOG_TIMEOUT_MS)
    {
      g_watchdog_permit = 0U;  /* 拒绝喂狗许可 */

      /* Stop 状态下不记录故障（这是正常的低功耗状态） */
      if (g_health.state != MONITOR_STATE_STOP)
      {
        g_health.watchdog_faults++;
        g_health.state = MONITOR_STATE_FAULT;
        MONITOR_LOG("[WATCHDOG] heartbeat stopped; software watchdog denied\r\n");
      }
      continue;  /* 不喂狗，等待硬件看门狗复位 */
    }

    /* ===== 场景 4：正常状态但心跳未更新 - 允许喂狗（容忍期内） ===== */
    /* 如果未超过超时时间，仍然允许喂狗 */
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

    /* ===== 步骤 2：生成周期请求 ===== */
    /* LED 心跳由 defaultTask 独占，周期任务只负责调度数据链路。 */
    request.cycle_id = ++g_health.cycles_started;  /* 全局递增周期 ID */
    request.timestamp_ticks = HAL_GetTick();       /* 记录周期开始时间戳 */
    g_health.state = MONITOR_STATE_ACQUIRE;

    /* ===== 步骤 3：发送周期请求到采集任务 ===== */
    if (osMessageQueuePut(g_cycle_request_queue, &request, 0U, 0U) != osOK)
    {
      /* 请求队列满：采集任务可能卡死（不应该发生，队列深度=2） */
      g_health.acquire_drops++;
      g_health.state = MONITOR_STATE_FAULT;
      MONITOR_LOG("[RTOS] cycle=%lu request queue full\r\n",
                  (unsigned long)request.cycle_id);
      MonitoringTasks_RearmRtcAlarm("request_drop");  /* 重设闹钟，继续下一周期 */
    }
    else
    {
      /* ===== 步骤 4：等待整个数据链路完成（采集→处理→上报） ===== */
      /* 数据流：
       *   1. AcquisitionTask 从 cycle_request_queue 取请求
       *   2. AcquisitionTask 采集完成后放入 sample_ready_queue
       *   3. ProcessingTask 从 sample_ready_queue 取块并处理
       *   4. ProcessingTask 处理完成后放入 result_queue
       *   5. ReportTask 从 result_queue 取结果并上报
       *   6. ReportTask 上报完成后释放 g_report_done_sem
       *
       * 等待超时：MONITOR_REPORT_WAIT_TIMEOUT_MS（12 秒）
       * 正常情况下，整个流程约 3-5 秒（采集 2.5 秒 + 处理 0.1 秒 + 上报 0.5 秒） */
      g_health.state = MONITOR_STATE_IDLE;
      if (g_report_done_sem != NULL)
      {
        if (osSemaphoreAcquire(g_report_done_sem,
                               MONITOR_REPORT_WAIT_TIMEOUT_MS) != osOK)
        {
          /* 上报超时：可能是 NRF24 发送卡死或任务链路中断 */
          g_health.report_drops++;
          g_health.state = MONITOR_STATE_FAULT;
          MONITOR_LOG("[RTOS] report completion timeout; stop denied\r\n");
          MonitoringTasks_RearmRtcAlarm("report_timeout");
        }
        else
        {
          /* ===== 步骤 5：尝试进入 Stop 模式（低功耗） ===== */
          /* MonitoringTasks_EnterStop 会：
           *   1. 检查门禁条件（看门狗许可、状态机状态、队列深度）
           *   2. 停止外设（ADC、TIM3、MPU6050）
           *   3. 设置 RTC 闹钟（下一周期唤醒）
           *   4. 等待 RTC 同步
           *   5. 进入 Stop 模式（暂停 CPU，保留 SRAM）
           *   6. 被 RTC 闹钟唤醒后恢复外设和传感器
           *
           * Stop 模式功耗：约 10-50 μA（相比运行模式的 20-40 mA）
           * 在长周期模式（如 300 秒）下，可大幅延长电池寿命 */
          if (MonitoringTasks_EnterStop() == 0U)
          {
            /* Stop 门禁拒绝：可能是看门狗故障、队列泄漏、或 FAULT 状态 */
            MONITOR_LOG("[RTOS] stop gate denied; system remains running\r\n");
            MonitoringTasks_RearmRtcAlarm("stop_gate_denied");
          }
          /* 如果成功进入 Stop，唤醒后会从这里继续执行，进入下一个循环 */
        }
      }
      else
      {
        /* 信号量未初始化：不应该发生（Create 阶段的错误） */
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
 * @note   初始化顺序：通信接口 → 传感器/模块 → 日志锁 → 事件组 → 队列 →
 *         块池预填充 → 任务创建 → 告警初始化
 */
void MonitoringTasks_Create(void)
{
  monitor_sample_block_t *block;
  const osMutexAttr_t log_mutex_attributes = {
    .name = "logMutex"
  };

  /* ═══════════════════════════════════════════════════════════════════
   * 第 1 步：初始化通信接口（SPI + I2C）
   * ═══════════════════════════════════════════════════════════════════
   * 必须在任务创建前完成，避免任务运行时驱动未就绪。
   * MonitoringBus_Init() 会初始化 SPI2 和 I2C1 的统计与错误恢复逻辑。
   */
  MonitoringBus_Init();

  /* ═══════════════════════════════════════════════════════════════════
   * 第 2 步：初始化传感器和模块
   * ═══════════════════════════════════════════════════════════════════
   * 初始化顺序（按依赖关系）：
   *   温度(DS18B20/1-Wire) → 振动(MPU6050/I2C) → 电流(ACS712/ADC)
   *   → 无线(NRF24L01/SPI) → 看门狗(IWDG)
   * 传感器初始化失败不会阻止系统启动（对应通道降级运行）。
   */
  MonitoringDrivers_Init();

  /* ═══════════════════════════════════════════════════════════════════
   * 第 3 步：创建同步资源（互斥量、事件组、信号量）
   * ═══════════════════════════════════════════════════════════════════
   * 这些资源必须在任务创建前就绪，否则任务运行时访问 NULL 句柄会崩溃。
   */

  /*
   * 【互斥量】g_log_mutex
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：保护 UART 日志输出，防止多任务并发调用导致乱码       │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 使用场景：所有任务调用 MONITOR_LOG 宏时                     │
   * │ 工作流程：                                                   │
   * │   任务 A: osMutexAcquire(g_log_mutex) → UART_Log →          │
   * │           osMutexRelease(g_log_mutex)                       │
   * │   任务 B: 等待任务 A 释放锁后才能输出                        │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_log_mutex = osMutexNew(&log_mutex_attributes);

  /*
   * 【事件组】g_monitor_cycle_event
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：RTC 闹钟中断通知 CycleTask 开始新周期                 │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 数据流向：RTC 中断 → 设置事件标志 → CycleTask 唤醒         │
   * │ 事件标志：MONITOR_EVENT_RTC_ALARM                           │
   * │ 使用位置：                                                   │
   * │   - HAL_RTC_AlarmAEventCallback: 设置标志                   │
   * │   - CycleTask: osEventFlagsWait 等待标志                    │
   * └─────────────────────────────────────────────────────────────┘
   */
  const osEventFlagsAttr_t cycle_event_attributes = {
    .name = "cycleEvent"
  };
  g_monitor_cycle_event = osEventFlagsNew(&cycle_event_attributes);

  /*
   * 【事件组】g_monitor_control_event
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：MPU6050 数据就绪中断通知 AcquisitionTask 读取 FIFO   │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 数据流向：MPU6050 中断 → 设置事件标志 → AcquisitionTask    │
   * │ 事件标志：MONITOR_EVENT_CAPTURE                             │
   * │ 为什么需要：避免在中断中访问 I2C（I2C 操作耗时长）          │
   * │ 使用位置：                                                   │
   * │   - HAL_GPIO_EXTI_Callback(PE2): 设置标志                   │
   * │   - AcquisitionTask: osEventFlagsWait 等待标志后读 FIFO    │
   * └─────────────────────────────────────────────────────────────┘
   */
  const osEventFlagsAttr_t control_event_attributes = {
    .name = "controlEvent"
  };
  g_monitor_control_event = osEventFlagsNew(&control_event_attributes);

  /*
   * 【信号量】g_report_done_sem（二值信号量，初始值 0）
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：ReportTask 通知 CycleTask 上报完成，可以进 Stop      │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 数据流向：ReportTask 完成 → Release 信号量 → CycleTask     │
   * │ 为什么需要：确保进 Stop 前本周期结果已上报完成              │
   * │ 工作流程：                                                   │
   * │   CycleTask: 发送周期请求 → Acquire(12s 超时) [等待]       │
   * │   ReportTask: 上报完成 → Release                            │
   * │   CycleTask: 收到信号 → 进入 Stop 模式                      │
   * │ 超时处理：12 秒未收到信号 → 记录错误 → 重设闹钟继续         │
   * └─────────────────────────────────────────────────────────────┘
   */
  const osSemaphoreAttr_t report_done_sem_attributes = {
    .name = "reportDoneSem"
  };
  g_report_done_sem = osSemaphoreNew(1U, 0U, &report_done_sem_attributes);

  /* ═══════════════════════════════════════════════════════════════════
   * 第 4 步：创建消息队列（4 个队列，构成数据流水线）
   * ═══════════════════════════════════════════════════════════════════
   * 队列必须在任务创建前就绪，任务启动后会立即调用 osMessageQueueGet。
   * 所有队列使用动态分配（控制块与存储区由 osMessageQueueNew 从堆申请）。
   * 队列深度统一为 2：支持双缓冲流水线（一个在处理，一个在采集）
   */

  /*
   * 【队列 1】g_cycle_request_queue
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：CycleTask 通知 AcquisitionTask 开始新周期采集         │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 队列深度：2                                                  │
   * │ 元素大小：sizeof(monitor_cycle_request_t)                   │
   * │ 传递数据：cycle_id（周期ID）+ timestamp_ticks（时间戳）     │
   * │ 数据流向：CycleTask → [队列] → AcquisitionTask              │
   * │                                                              │
   * │ 生产者：CycleTask（收到 RTC 闹钟事件后）                    │
   * │ 消费者：AcquisitionTask（阻塞等待，开始采集）               │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_cycle_request_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_request_t),
    "cycleRequestQ");

  /*
   * 【队列 2】g_sample_free_queue（采样块池）
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：管理空闲采样块，实现块池化（避免动态分配）            │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 队列深度：2                                                  │
   * │ 元素大小：sizeof(monitor_sample_block_t *)（指针）          │
   * │ 块大小：~8KB（1024点×3轴振动 + 1024点电流 + 温度）          │
   * │                                                              │
   * │ 块流转：初始化→free队列→AcquisitionTask取块→填充→          │
   * │         sample_ready队列→ProcessingTask处理→归还free队列    │
   * │                                                              │
   * │ 为什么用块池：采样块很大，避免 malloc/free，固定 2 个块     │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_sample_free_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    "sampleFreeQ");

  /*
   * 【队列 3】g_sample_ready_queue
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：AcquisitionTask 传递采样块给 ProcessingTask           │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 队列深度：2                                                  │
   * │ 元素大小：sizeof(monitor_sample_block_t *)（指针）          │
   * │ 数据流向：AcquisitionTask → [队列] → ProcessingTask         │
   * │                                                              │
   * │ 采样块内容：温度1点 + 振动1024×3轴 + 电流1024点             │
   * │                                                              │
   * │ 生产者：AcquisitionTask（采集完成后放入）                   │
   * │ 消费者：ProcessingTask（阻塞等待，执行FFT/RMS/告警）        │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_sample_ready_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_sample_block_t *),
    "sampleReadyQ");

  /*
   * 【队列 4】g_result_queue
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 用途：ProcessingTask 传递处理结果给 ReportTask              │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 队列深度：2                                                  │
   * │ 元素大小：sizeof(monitor_cycle_result_t)（值拷贝）          │
   * │ 数据流向：ProcessingTask → [队列] → ReportTask              │
   * │                                                              │
   * │ 结果内容：温度+振动RMS+电流+告警状态+处理时间               │
   * │                                                              │
   * │ 生产者：ProcessingTask（算法处理完成后）                    │
   * │ 消费者：ReportTask（阻塞等待，UART+NRF24上报）              │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_result_queue = MonitoringTasks_CreateQueue(
    MONITOR_QUEUE_DEPTH,
    sizeof(monitor_cycle_result_t),
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

  /* ═══════════════════════════════════════════════════════════════════
   * 第 5 步：初始化采样块池（预填充空闲队列）
   * ═══════════════════════════════════════════════════════════════════
   * 将所有空块指针预先放入 sample_free_queue，供 acquisition_task 取用。
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

  /* ═══════════════════════════════════════════════════════════════════
   * 第 6 步：创建 6 个任务（按优先级从高到低）
   * ═══════════════════════════════════════════════════════════════════
   * 任务优先级决定抢占关系：高(48) > 中高(40) > 中(24) > 中低(16) > 低(8)
   * 任务 TCB 与栈由 osThreadNew 从 FreeRTOS 堆动态分配，创建一次后常驻；
   * 仅采样块池保持静态分配（固定 SRAM 预算，避免热路径 malloc）。
   */

  /*
   * 【任务 1】AcquisitionTask - 采集任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：48（osPriorityHigh）- 最高                          │
   * │ 栈大小：1024 字节                                            │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：周期性采集温度、振动、电流三通道数据                  │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osMessageQueueGet(cycle_request_queue) [阻塞等待]      │
   * │   2. osMessageQueueGet(sample_free_queue) [取空闲块]        │
   * │   3. MonitoringAcquisition_Capture(block) [采集 2.5s]       │
   * │      ├─ DS18B20 温度（750 ms）                              │
   * │      ├─ MPU6050 振动（1280 ms，1024点×3轴）                │
   * │      └─ ADC 电流（1024 ms，1024点）                         │
   * │   4. osMessageQueuePut(sample_ready_queue) [交给处理]       │
   * │   5. 循环到步骤 1                                            │
   * │                                                              │
   * │ 为什么优先级最高：确保采集窗口不被打断，保证数据完整性      │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_acquisition_task = MonitoringTasks_CreateThread(
    "acquisition_task", MonitoringTasks_AcquisitionTask, osPriorityHigh,
    256U * sizeof(StackType_t));

  /*
   * 【任务 2】ProcessingTask - 处理任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：40（osPriorityAboveNormal）- 中高                   │
   * │ 栈大小：1024 字节                                            │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：执行信号处理算法和告警评估                             │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osMessageQueueGet(sample_ready_queue) [阻塞等待]       │
   * │   2. MonitoringAlgorithm_Process(block, &result) [~100ms]   │
   * │      ├─ 振动：去直流→Q15→汉宁窗→FFT→RMS→频段能量            │
   * │      ├─ 电流：去直流→Q15→RMS                                │
   * │      └─ 告警：评估各通道→更新状态机                         │
   * │   3. osMessageQueuePut(result_queue) [交给上报]             │
   * │   4. osMessageQueuePut(sample_free_queue) [归还块]          │
   * │   5. 循环到步骤 1                                            │
   * │                                                              │
   * │ 为什么优先级中高：FFT 计算需要优先完成，但可被采集打断      │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_processing_task = MonitoringTasks_CreateThread(
    "processing_task", MonitoringTasks_ProcessingTask, osPriorityAboveNormal,
    256U * sizeof(StackType_t));

  /*
   * 【任务 3】ReportTask - 上报任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：16（osPriorityBelowNormal）- 中低                   │
   * │ 栈大小：768 字节                                             │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：通过 UART 和 NRF24 上报处理结果                        │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osMessageQueueGet(result_queue) [阻塞等待]             │
   * │   2. MONITOR_LOG(...) [UART 输出详细日志，~100ms]           │
   * │   3. MonitoringNrf24_SendPayload(...) [无线发送，~5ms]      │
   * │   4. osSemaphoreRelease(report_done_sem) [通知完成]         │
   * │   5. 循环到步骤 1                                            │
   * │                                                              │
   * │ 为什么优先级中低：上报可以延后，不影响采集和处理            │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_report_task = MonitoringTasks_CreateThread(
    "report_task", MonitoringTasks_ReportTask, osPriorityBelowNormal,
    192U * sizeof(StackType_t));

  /*
   * 【任务 4】CycleTask - 周期协调任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：24（osPriorityNormal）- 中                          │
   * │ 栈大小：1024 字节                                            │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：周期协调 + Stop 模式管理（低功耗核心）                │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osEventFlagsWait(RTC_ALARM) [阻塞等待 RTC 中断]        │
   * │   2. 生成 cycle_request（cycle_id + timestamp）             │
   * │   3. osMessageQueuePut(cycle_request_queue) [触发采集]      │
   * │   4. osSemaphoreAcquire(report_done_sem, 12s) [等待上报]    │
   * │   5. MonitoringTasks_EnterStop() [进入 Stop 模式]           │
   * │      ├─ 停止外设（ADC/TIM3/MPU6050）                        │
   * │      ├─ 设置 RTC 闹钟（10s测试 / 300s生产）                │
   * │      ├─ HAL_PWR_EnterSTOPMode() [CPU停止，功耗10-50μA]      │
   * │      └─ 被 RTC 唤醒后恢复外设                               │
   * │   6. 循环到步骤 1                                            │
   * │                                                              │
   * │ 为什么优先级中：协调角色，不需要抢占采集/处理               │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_cycle_task = MonitoringTasks_CreateThread(
    "cycle_task", MonitoringTasks_RunCycleTask, osPriorityNormal,
    256U * sizeof(StackType_t));

  /*
   * 【任务 5】HealthTask - 健康监测任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：8（osPriorityLow）- 低                              │
   * │ 栈大小：768 字节                                             │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：定期输出系统健康日志（监测，不影响主流程）            │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osDelay(10000) [延时 10 秒]                            │
   * │   2. uxTaskGetStackHighWaterMark() [读取各任务栈水位]       │
   * │   3. MonitoringSpi_GetStatus() [读取 SPI 统计]              │
   * │   4. osMessageQueueGetCount() [读取队列深度]                │
   * │   5. MONITOR_LOG("[HEALTH] ...") [输出健康日志]             │
   * │   6. 循环到步骤 1                                            │
   * │                                                              │
   * │ 输出内容：栈水位、队列深度、错误计数、Stop 次数等           │
   * │ 为什么优先级低：只做监测，不参与数据流，可随时被打断        │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_health_task = MonitoringTasks_CreateThread(
    "health_task", MonitoringTasks_HealthTask, osPriorityLow,
    192U * sizeof(StackType_t));

  /*
   * 【任务 6】WatchdogTask - 看门狗任务
   * ┌─────────────────────────────────────────────────────────────┐
   * │ 优先级：8（osPriorityLow）- 低                              │
   * │ 栈大小：640 字节                                             │
   * ├─────────────────────────────────────────────────────────────┤
   * │ 职责：监测任务心跳，决定是否喂硬件看门狗                    │
   * │                                                              │
   * │ 工作流程：                                                   │
   * │   1. osDelay(1000) [延时 1 秒]                              │
   * │   2. 检查各任务心跳是否更新                                  │
   * │      ├─ acquisition_heartbeat                               │
   * │      ├─ processing_heartbeat                                │
   * │      └─ report_heartbeat                                    │
   * │   3. 如果心跳正常或处于 IDLE/Stop 状态：                    │
   * │      └─ g_watchdog_permit = 1, 喂狗                         │
   * │   4. 如果心跳超时（10秒）或 FAULT 状态：                    │
   * │      └─ g_watchdog_permit = 0, 拒绝喂狗 → 硬件复位         │
   * │   5. 循环到步骤 1                                            │
   * │                                                              │
   * │ 为什么需要：防止任务卡死，最后一道安全防线                  │
   * │ 为什么优先级低：监测功能，不参与数据流                      │
   * └─────────────────────────────────────────────────────────────┘
   */
  g_watchdog_task = MonitoringTasks_CreateThread(
    "watchdog_task", MonitoringTasks_WatchdogTask, osPriorityLow,
    160U * sizeof(StackType_t));

  /* ═══════════════════════════════════════════════════════════════════
   * 失败检查：6 个任务全部创建成功才继续
   * ═══════════════════════════════════════════════════════════════════
   * 动态分配会真实失败（堆不足时返回 NULL），打印剩余堆便于定位。
   */
  if (g_acquisition_task == NULL || g_processing_task == NULL ||
      g_report_task == NULL || g_health_task == NULL || g_cycle_task == NULL ||
      g_watchdog_task == NULL)
  {
    UART_Log("[RTOS] task pipeline thread creation failed; free heap=%lu bytes\r\n",
             (unsigned long)xPortGetFreeHeapSize());
    g_health.state = MONITOR_STATE_FAULT;
    return;
  }

  /* ═══════════════════════════════════════════════════════════════════
   * 第 7 步：初始化应用层状态（任务尚未运行）
   * ═══════════════════════════════════════════════════════════════════
   * 任务已创建但尚未运行（调度器在 main.c 的 osKernelStart 后才启动），
   * 此时可以安全地初始化应用层状态，无并发问题。
   */

  /*
   * MonitoringAlerts_Reset():
   *   - 告警状态机复位（所有通道进入 NORMAL 状态）
   *   - 清除连续超限/恢复计数器
   *   - 保证系统启动时无历史告警残留
   */
  MonitoringAlerts_Reset();

  /*
   * 看门狗初始许可：
   *   - g_watchdog_permit = 1：允许 watchdog_task 开始喂狗
   *   - g_watchdog_last_progress_tick：记录初始时间戳
   *   - 任务启动后，watchdog_task 会周期性检查心跳并喂狗
   */
  g_watchdog_last_progress_tick = HAL_GetTick();
  g_watchdog_permit = 1U;

  /*
   * 启动完成日志：
   *   - 打印到 UART，确认所有任务创建成功
   *   - 之后 main.c 调用 osKernelStart() 启动调度器
   *   - 从此刻起，任务开始按优先级调度运行
   */
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
