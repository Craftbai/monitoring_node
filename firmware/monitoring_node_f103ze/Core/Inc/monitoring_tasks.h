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

/* ========================================================================
 * 固件运行参数与事件标志
 * ======================================================================== */

/* 配置版本号：用于上报结果时区分参数快照，主版本.次版本（16.16 分割） */
#define MONITOR_CONFIG_VERSION       0x00010001UL

/* 采样块池大小：同时在采集/处理/上报链路中流转的最大块数 */
#define MONITOR_SAMPLE_POOL_SIZE     2U

/* 振动通道数：MPU6050 三轴加速度 X/Y/Z */
#define MONITOR_VIBRATION_AXES       3U

/* 振动采样点数：800 Hz × 1.28 秒窗口，FFT 要求 2^N */
#define MONITOR_VIBRATION_SAMPLES    1024U

/* 电流采样点数：1600 Hz × 0.64 秒窗口，与振动窗口部分重叠 */
#define MONITOR_CURRENT_SAMPLES      1024U

/* 队列深度：必须 >= POOL_SIZE，保证块池与空闲队列一一对应 */
#define MONITOR_QUEUE_DEPTH          2U

/* 测试模式：1=5秒快速周期便于串口观察，0=300秒生产周期 */
#define MONITOR_TEST_MODE            0U
#define MONITOR_TEST_INTERVAL_SEC    5U
#define MONITOR_PRODUCTION_PERIOD_SEC 300U

#if MONITOR_TEST_MODE
#define MONITOR_CYCLE_INTERVAL_SEC   MONITOR_TEST_INTERVAL_SEC
#else
#define MONITOR_CYCLE_INTERVAL_SEC   MONITOR_PRODUCTION_PERIOD_SEC
#endif

/* RTC 闹钟等待超时：周期间隔 + 10 秒容错，超时后主动重设闹钟恢复 */
#define MONITOR_CYCLE_WAIT_TIMEOUT_MS ((MONITOR_CYCLE_INTERVAL_SEC + 10U) * 1000U)

/* 上报完成等待超时：采集→处理→上报最坏链路时长，超时则放弃本周期 Stop */
#define MONITOR_REPORT_WAIT_TIMEOUT_MS 12000U

/* FreeRTOS 事件标志位：用于任务间同步与中断通知 */
#define MONITOR_EVENT_RTC_ALARM      (1UL << 0)  /* RTC 闹钟触发新周期 */
#define MONITOR_EVENT_CAPTURE        (1UL << 1)  /* MPU6050 数据就绪中断 */
#define MONITOR_EVENT_REPORT_DONE    (1UL << 2)  /* 上报完成（保留） */
#define MONITOR_EVENT_STOP_WAKE      (1UL << 3)  /* Stop 唤醒（保留） */

/* 采集块池和空闲队列必须一一对应，否则可能出现块永久丢失。 */
#if MONITOR_SAMPLE_POOL_SIZE > MONITOR_QUEUE_DEPTH
#error "MONITOR_QUEUE_DEPTH must cover MONITOR_SAMPLE_POOL_SIZE"
#endif

/* ========================================================================
 * 采样块标志位：记录采集过程中的异常与传感器状态
 * ======================================================================== */
#define MONITOR_SAMPLE_FLAG_NO_SENSOR       (1UL << 0)  /* 所有通道均无有效传感器 */
#define MONITOR_SAMPLE_FLAG_TEMP_INVALID    (1UL << 1)  /* 温度通道无效 */
#define MONITOR_SAMPLE_FLAG_VIB_INVALID     (1UL << 2)  /* 振动通道无效 */
#define MONITOR_SAMPLE_FLAG_CURRENT_INVALID (1UL << 3)  /* 电流通道无效 */
#define MONITOR_SAMPLE_FLAG_TIMEOUT         (1UL << 4)  /* 采集窗口超时 */
#define MONITOR_SAMPLE_FLAG_OVERFLOW        (1UL << 5)  /* FIFO/DMA 溢出 */
#define MONITOR_SAMPLE_FLAG_SATURATION      (1UL << 6)  /* ADC 饱和(≥4095) */
#define MONITOR_SAMPLE_FLAG_TEMP_MISSING    (1UL << 7)  /* DS18B20 未响应 */
#define MONITOR_SAMPLE_FLAG_VIB_MISSING     (1UL << 8)  /* MPU6050 未响应 */
#define MONITOR_SAMPLE_FLAG_CURRENT_MISSING (1UL << 9)  /* ACS712 未接入 */

/* ========================================================================
 * 错误标志位：汇总周期内的异常类别，用于诊断和结果上报
 * ======================================================================== */
#define MONITOR_ERROR_NO_SENSOR             (1UL << 0)  /* 传感器缺失 */
#define MONITOR_ERROR_ACQUIRE_DROP          (1UL << 1)  /* 采集队列满/块池耗尽 */
#define MONITOR_ERROR_PROCESS_DROP          (1UL << 2)  /* 处理队列满 */
#define MONITOR_ERROR_REPORT_DROP           (1UL << 3)  /* 上报队列满/超时 */
#define MONITOR_ERROR_TEMP                  (1UL << 4)  /* 温度读取失败 */
#define MONITOR_ERROR_VIBRATION             (1UL << 5)  /* 振动采集失败 */
#define MONITOR_ERROR_CURRENT               (1UL << 6)  /* 电流采集失败 */
#define MONITOR_ERROR_ALGORITHM             (1UL << 7)  /* 算法饱和/溢出 */
#define MONITOR_ERROR_STOP_GATE             (1UL << 8)  /* Stop 门禁失败 */
#define MONITOR_ERROR_WATCHDOG              (1UL << 9)  /* 看门狗超时 */

/* ========================================================================
 * 有效通道掩码：标识哪些通道的特征值可用于告警判定
 * ======================================================================== */
#define MONITOR_VALID_TEMPERATURE           (1UL << 0)  /* 温度有效 */
#define MONITOR_VALID_VIBRATION             (1UL << 1)  /* 振动三轴有效 */
#define MONITOR_VALID_CURRENT               (1UL << 2)  /* 电流有效 */

/* ========================================================================
 * 周期状态机：每次 RTC 唤醒生成唯一 cycle_id，按固定流程推进
 * BOOT → SELF_TEST → IDLE → ACQUIRE → PROCESS → EVALUATE → REPORT
 *   → PREPARE_STOP → STOP → (RTC 唤醒回到 IDLE)
 * 异常时进入 FAULT，由软件看门狗决定是否重启或保持运行
 * ======================================================================== */
typedef enum
{
  MONITOR_STATE_BOOT = 0,         /* 启动阶段，外设初始化 */
  MONITOR_STATE_SELF_TEST,        /* 自检（预留），当前直接进 IDLE */
  MONITOR_STATE_IDLE,             /* 空闲等待 RTC 闹钟或处理完成 */
  MONITOR_STATE_ACQUIRE,          /* 采集任务正在执行 */
  MONITOR_STATE_PROCESS,          /* 处理任务正在执行 */
  MONITOR_STATE_EVALUATE,         /* 告警评估（由算法内联完成） */
  MONITOR_STATE_REPORT,           /* 上报任务正在执行 */
  MONITOR_STATE_PREPARE_STOP,     /* Stop 前门禁检查 */
  MONITOR_STATE_STOP,             /* 已进入 Stop 模式 */
  MONITOR_STATE_FAULT             /* 不可恢复故障，停止新周期 */
} monitor_state_t;

/* ========================================================================
 * 周期请求：RTC 闹钟触发后，cycle_task 生成请求并放入队列
 * ======================================================================== */
typedef struct
{
  uint32_t cycle_id;          /* 单调递增的周期 ID，用于关联采样块与结果 */
  uint32_t timestamp_ticks;   /* 请求发起时的 HAL_GetTick() 值 */
} monitor_cycle_request_t;

/* ========================================================================
 * 采样块：采集任务从块池取得空块，填充后交给处理任务
 * 所有权流转：free_queue → acquisition_task → ready_queue → processing_task → free_queue
 * ======================================================================== */
typedef struct
{
  uint32_t cycle_id;                                           /* 所属周期 ID */
  uint32_t sequence;                                           /* 块序号，用于检测丢块 */
  uint32_t timestamp_ticks;                                    /* 采集开始时刻 */
  uint32_t flags;                                              /* MONITOR_SAMPLE_FLAG_* 位图 */
  uint32_t channel_mask;                                       /* 期望采集的通道掩码 */
  uint16_t vibration_rate_hz;                                  /* 实际振动采样率 */
  uint16_t vibration_sample_count;                             /* 实际振动样本数 */
  uint16_t current_rate_hz;                                    /* 实际电流采样率 */
  uint16_t current_sample_count;                               /* 实际电流样本数 */
  int16_t temperature_centi;                                   /* 温度，单位 0.01°C */
  uint16_t current_raw;                                        /* 电流窗口均值 ADC 原始值 */
  int16_t vibration[MONITOR_VIBRATION_AXES][MONITOR_VIBRATION_SAMPLES]; /* X/Y/Z 加速度 */
  uint16_t current[MONITOR_CURRENT_SAMPLES];                   /* 电流 ADC 原始值序列 */
} monitor_sample_block_t;

/* ========================================================================
 * 周期结果：处理任务完成特征提取与告警评估后，放入结果队列供上报
 * ======================================================================== */
typedef struct
{
  uint32_t cycle_id;                                     /* 所属周期 ID */
  uint32_t config_version;                               /* 配置版本（阈值快照标识） */
  uint32_t valid_mask;                                   /* MONITOR_VALID_* 位图 */
  uint32_t error_flags;                                  /* MONITOR_ERROR_* 位图 */
  uint32_t sample_flags;                                 /* 采样块的 flags 副本 */
  uint16_t vibration_sample_count;                       /* 振动实际样本数 */
  uint16_t current_sample_count;                         /* 电流实际样本数 */
  uint16_t vibration_rate_hz;                            /* 振动采样率 */
  uint16_t current_rate_hz;                              /* 电流采样率 */
  int16_t temperature_centi;                             /* 温度，单位 0.01°C */
  uint16_t current_mean_raw;                             /* 电流均值 ADC 原始值 */
  uint16_t current_rms_raw;                              /* 电流 RMS ADC 原始值 */
  int32_t current_milliamp;                              /* 电流有效值，单位 mA */
  int32_t vibration_rms_mg[MONITOR_VIBRATION_AXES];      /* 振动 RMS，单位 mg (毫重力) */
  int32_t vibration_peak_to_peak_mg[MONITOR_VIBRATION_AXES]; /* 峰峰值，单位 mg */
  uint32_t vibration_crest_milli[MONITOR_VIBRATION_AXES]; /* 峰值因子×1000 */
  uint32_t vibration_band_energy_milli[MONITOR_VIBRATION_AXES]; /* 频带能量×1000 */
  uint32_t alert_mask;                                   /* 当前激活的告警位图 */
  uint32_t alert_states;                                 /* 5 通道告警状态（每通道 2bit） */
  uint8_t alert_counts[MONITOR_VIBRATION_AXES + 2U];     /* 连续超限/恢复计数 */
  uint32_t algorithm_saturation_count;                   /* 算法饱和次数 */
  uint32_t processing_time_ms;                           /* 处理耗时 */
  monitor_state_t state;                                 /* 周期完成时的状态 */
} monitor_cycle_result_t;

/* ========================================================================
 * 健康统计：由 health_task 定期更新，供 UART 日志输出和诊断
 * ======================================================================== */
typedef struct
{
  volatile uint32_t cycles_started;              /* RTC 闹钟触发次数 */
  volatile uint32_t samples_ready;               /* 成功放入 ready_queue 的块数 */
  volatile uint32_t samples_processed;           /* 成功处理的块数 */
  volatile uint32_t reports_sent;                /* 成功上报的结果数 */
  volatile uint32_t acquire_drops;               /* 采集侧丢块次数 */
  volatile uint32_t process_drops;               /* 处理侧丢块次数 */
  volatile uint32_t report_drops;                /* 上报侧丢失次数 */
  volatile uint32_t rtc_wait_timeouts;           /* RTC 闹钟等待超时次数 */
  volatile uint32_t rtc_alarm_errors;            /* RTC 闹钟设置失败次数 */
  volatile uint32_t dma_half_events;             /* ADC DMA 半传输中断次数 */
  volatile uint32_t dma_full_events;             /* ADC DMA 全传输中断次数 */
  volatile uint32_t sensor_errors;               /* 传感器缺失次数 */
  volatile uint32_t watchdog_faults;             /* 软件看门狗超时次数 */
  volatile uint32_t stop_entries;                /* 进入 Stop 次数 */
  volatile uint32_t stop_wakeups;                /* Stop 唤醒次数 */
  volatile uint32_t acquisition_heartbeat;       /* 采集任务心跳计数 */
  volatile uint32_t processing_heartbeat;        /* 处理任务心跳计数 */
  volatile uint32_t report_heartbeat;            /* 上报任务心跳计数 */
  volatile uint32_t nrf24_reports;               /* NRF24 成功发送次数 */
  volatile uint32_t nrf24_errors;                /* NRF24 发送失败次数 */
  volatile uint16_t acquisition_stack_free_words; /* 采集任务栈剩余字数 */
  volatile uint16_t processing_stack_free_words;  /* 处理任务栈剩余字数 */
  volatile uint16_t report_stack_free_words;      /* 上报任务栈剩余字数 */
  volatile uint16_t health_stack_free_words;      /* 健康任务栈剩余字数 */
  volatile uint16_t cycle_stack_free_words;       /* 周期任务栈剩余字数 */
  volatile uint16_t watchdog_stack_free_words;    /* 看门狗任务栈剩余字数 */
  volatile monitor_state_t state;                 /* 当前全局状态 */
} monitor_health_stats_t;

/* ========================================================================
 * 全局事件接口：由中断或任务发布事件，cycle_task 和 acquisition_task 等待
 * ======================================================================== */

/* RTC 闹钟事件标志，由 HAL_RTC_AlarmAEventCallback 发布 */
extern osEventFlagsId_t g_monitor_cycle_event;

/* 采集控制事件标志，用于 MPU6050 中断通知和软件看门狗 */
extern osEventFlagsId_t g_monitor_control_event;

/* ========================================================================
 * 外部依赖接口：由 main.c 提供
 * ======================================================================== */

/* RTC 闹钟设置：interval_sec 秒后触发 ALARM_A 中断，返回 HAL 状态 */
HAL_StatusTypeDef RTC_SetNextAlarm(uint32_t interval_sec);

/* ========================================================================
 * 调度装配接口：在 freertos.c 的 MX_FREERTOS_Init 中调用
 * ======================================================================== */

/* 创建固定块池、队列、事件组和 6 个应用任务，初始化驱动层 */
void MonitoringTasks_Create(void);

/* 周期协调任务入口：等待 RTC 闹钟 → 发起采集 → 等待上报 → 进入 Stop */
void MonitoringTasks_RunCycleTask(void *argument);

/* ========================================================================
 * NRF24 回调接口：由 monitoring_drivers 使用，控制 CSN/CE 引脚
 * ======================================================================== */

/* NRF24 片选控制（CSN 引脚） */
void MonitoringTasks_Nrf24ChipSelect(uint8_t active);

/* NRF24 使能控制（CE 引脚） */
void MonitoringTasks_Nrf24ChipEnable(uint8_t active);

/* ========================================================================
 * 中断回调接口：由驱动层和 HAL 回调调用，只发布事件不阻塞
 * ======================================================================== */

/* MPU6050 数据就绪中断（PE2/EXTI2），发布 MONITOR_EVENT_CAPTURE 事件 */
void MonitoringTasks_OnMpuDataReady(void);

/* ADC DMA 半传输中断，更新健康统计 */
void MonitoringTasks_OnAdcHalfComplete(void);

/* ADC DMA 全传输中断，更新健康统计 */
void MonitoringTasks_OnAdcComplete(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITORING_TASKS_H */
