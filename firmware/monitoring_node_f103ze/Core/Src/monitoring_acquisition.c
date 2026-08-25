/* =============================================================================
 * monitoring_acquisition.c - 三通道采集服务层实现
 *
 * 采集架构：
 *   本模块屏蔽底层驱动差异，为上层提供统一的"采样块"接口。每次调用
 *   MonitoringAcquisition_Capture() 会并行采集三个通道，填充一个固定
 *   大小的采样块（温度 1 点、振动 3×1024 点、电流 1024 点）。
 *
 * 并行采集策略（避免串行等待浪费时间）：
 *   1. 温度（DS18B20/1-Wire）：
 *      - 转换时间 750ms，先启动转换，让它在后台运行
 *      - 在等待振动/电流采集期间，每 5ms 轮询一次转换状态
 *      - 800ms 超时后强制标记为失败
 *
 *   2. 振动（MPU6050/I2C + 1024 字节 FIFO）：
 *      - 配置为 1kHz 原始采样，DLPF 滤波后数据进 FIFO
 *      - PE2/EXTI2 中断只发布事件，I2C 读取留给任务上下文
 *      - 每次轮询尽量排空 FIFO（避免 1024 字节溢出）
 *      - 每 5 个原始样本保留前 4 个 → 800Hz 等效窗口（1280 → 1024）
 *
 *   3. 电流（ACS712/ADC1 + DMA + TIM3 触发）：
 *      - TIM3 配置为 1600Hz 触发频率
 *      - ADC1 DMA 模式，目标 1024 点
 *      - 半传输中断（512 点）+ 全传输中断（1024 点）
 *      - 任务上下文中 memcpy 到采样块（避免在中断中操作块池）
 *
 * 完成条件：
 *   - 三个通道都完成（温度转换完成、振动达到 1024 点、电流 DMA 完成）
 *   - 或者超时（2.5 秒）
 *   - 完成后设置每个通道的有效标志（INVALID/MISSING/OVERFLOW/SATURATION）
 *
 * Stop 模式支持：
 *   - Stop(): 停止所有外设（TIM3/ADC/MPU6050），清理中断标志
 *   - Resume(): Stop 唤醒后，DeInit → Init → 校准，恢复外设到初始状态
 * ============================================================================= */

#include "monitoring_acquisition.h"

#include "adc.h"
#include "monitoring_ds18b20.h"
#include "monitoring_drivers.h"
#include "i2c.h"
#include "monitoring_mpu6050.h"
#include "monitoring_nrf24.h"
#include "tim.h"

#include <string.h>

/* ===== 内部常量 ===== */
/* ADC DMA 缓冲区大小（与电流采样点数一致） */
#define MONITOR_ADC_DMA_COUNT       MONITOR_CURRENT_SAMPLES

/* ADC DMA 半传输点数（用于半传输中断后的 memcpy） */
#define MONITOR_ADC_HALF_SAMPLES    (MONITOR_ADC_DMA_COUNT / 2U)

/* 采集窗口超时（2.5 秒，足够容纳最坏情况：温度 750ms + 振动 1.28s + 电流 0.64s） */
#define MONITOR_CAPTURE_TIMEOUT_MS  2500U

/* 振动源样本数：1kHz × 1.28s = 1280 点，每 5 点保留前 4 点 → 1024 有效点 */
#define MONITOR_VIBRATION_SOURCE_SAMPLES \
  ((MONITOR_VIBRATION_SAMPLES * 5U) / 4U)

/* ===== 静态变量（DMA 缓冲区与状态标志） ===== */

/* ===== 静态变量（DMA 缓冲区与状态标志） ===== */

#if MONITOR_CURRENT_SENSOR_ENABLED
/* ADC DMA 目标缓冲区（1024 点 × 2 字节，4 字节对齐） */
static uint16_t g_adc_dma_buffer[MONITOR_ADC_DMA_COUNT]
  __attribute__((aligned(4)));
#endif

/* ADC DMA 中断标志（由 HAL 回调在中断上下文中设置，任务轮询） */
static volatile uint8_t g_adc_half_ready;  /* 半传输中断已触发 */
static volatile uint8_t g_adc_full_ready;  /* 全传输中断已触发 */
static volatile uint8_t g_adc_error;       /* DMA 错误中断已触发 */

#if MONITOR_CURRENT_SENSOR_ENABLED
/* ADC 校准状态（Init 时执行一次，Resume 时重新校准） */
static uint8_t g_adc_calibrated;

/* ADC 捕获活动标志（用于 Stop 门禁检查：只停止确实启动过的 DMA） */
static uint8_t g_adc_capture_active;
#endif

/* ===== 内部辅助函数 ===== */

#if MONITOR_CURRENT_SENSOR_ENABLED
/**
 * @brief  计算 ADC 采样序列的算术平均值
 * @param  data: ADC 原始值数组
 * @param  count: 样本数
 * @return 平均值（整数）
 * @note   用于计算电流窗口的直流分量（零点参考）
 */
static uint16_t MonitoringAcquisition_Average(const uint16_t *data, uint16_t count)
{
  uint64_t sum = 0U;

  for (uint16_t i = 0U; i < count; i++)
  {
    sum += data[i];
  }
  return count == 0U ? 0U : (uint16_t)(sum / count);
}
#endif

/* ===== 公开 API 实现 ===== */

/**
 * @brief  初始化采集层（ADC 校准、MPU6050 初始化）
 * @note   在 MonitoringTasks_Create() 中调用，调度器启动前完成
 * @note   传感器初始化失败不会阻止任务创建，只影响对应通道有效标志
 */
/**
 * @brief  执行一次三通道采集（温度/振动/电流）
 * @param  block: 采样块指针（由调用者从块池取得）
 * @return 采样块的 flags 字段（MONITOR_SAMPLE_FLAG_* 位图）
 *
 * @note   采集流程分为 3 个阶段：
 *
 * 【阶段 1：启动三个通道的采集】
 *   1. 温度：DS18B20_StartTemperatureConversion()，启动后台转换（750ms）
 *   2. 振动：MPU6050_StartCapture()，启动 FIFO 填充（1kHz 采样）
 *   3. 电流：HAL_ADC_Start_DMA() + HAL_TIM_Base_Start(&htim3)，启动定时触发
 *
 * 【阶段 2：轮询三个通道的完成状态】
 *   - 主循环每次迭代检查三个 done 标志，全部完成或超时后退出
 *   - 温度：每 5ms 轮询 DS18B20_ConversionReady()，800ms 超时
 *   - 振动：等待 PE2 中断或 1ms 超时，读 FIFO 直到 1280 样本（→ 1024 有效）
 *   - 电流：等待 DMA 半传输/全传输中断，中断触发后 memcpy 到块
 *
 * 【阶段 3：收尾与有效标志设置】
 *   - 读取温度最终结果（DS18B20_ReadTemperatureRaw）
 *   - 停止所有外设（TIM3/ADC/MPU6050）
 *   - 根据实际采集结果清除或保留 INVALID 标志
 *   - 如果三个通道都无效，设置 NO_SENSOR 标志
 */
uint32_t MonitoringAcquisition_Capture(monitor_sample_block_t *block)
{
  uint32_t start_tick;
  uint16_t fifo_count;
  mpu6050_sample_t sample;
  monitoring_status_t temperature_status;
  int16_t temperature_raw;
  uint8_t temperature_conversion_started = 0U;
  uint8_t mpu_capture_started = 0U;
  uint8_t adc_capture_started = 0U;
#if MONITOR_CURRENT_SENSOR_ENABLED
  uint8_t adc_half_copied = 0U;
  uint8_t adc_full_copied = 0U;
#endif
  uint8_t vibration_done;
  uint8_t current_done;
  uint8_t temperature_done;
  uint32_t temperature_start_tick = 0U;
  uint32_t temperature_poll_tick = 0U;
  uint16_t vibration_source_count = 0U;

  if (block == NULL)
  {
    return MONITOR_SAMPLE_FLAG_TIMEOUT;
  }

  /* 初始化采样块元数据 */
  block->flags = 0U;
  block->vibration_rate_hz = MPU6050_SAMPLE_RATE_HZ;
  block->vibration_sample_count = 0U;
  block->current_rate_hz = 1600U;
  block->current_sample_count = 0U;

  /* ===== 阶段 1：启动三个通道的采集 ===== */

  /* 温度：先启动转换，随后并行采集其他通道，避免把 750 ms 转换时间放在前面。
   * 如果启动失败（传感器未响应），直接标记为 INVALID + MISSING，
   * temperature_done 置 1 表示此通道不再参与后续轮询。 */
  temperature_status = DS18B20_StartTemperatureConversion();
  temperature_conversion_started = (temperature_status == MONITORING_OK) ? 1U : 0U;
  temperature_done = (temperature_conversion_started == 0U) ? 1U : 0U;
  temperature_start_tick = HAL_GetTick();
  temperature_poll_tick = temperature_start_tick;
  if (temperature_conversion_started == 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_TEMP_INVALID | MONITOR_SAMPLE_FLAG_TEMP_MISSING;
  }

  /* 振动：启动 MPU6050 FIFO 采集。
   * 如果启动失败（传感器未初始化或通信错误），标记为 INVALID + MISSING。 */
  if (!MonitoringDrivers_IsReady(DRIVER_MPU6050) || MPU6050_StartCapture() != MONITORING_OK)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_VIB_MISSING;
  }
  else
  {
    mpu_capture_started = 1U;
  }

  /* 电流：启动 ADC DMA + TIM3 定时触发。
   * 启动条件：ADC 已校准 && DMA 启动成功 && TIM3 启动成功
   * 如果任一条件失败，标记为 INVALID + MISSING，并清理已启动的外设。 */
#if MONITOR_CURRENT_SENSOR_ENABLED
  g_adc_half_ready = 0U;
  g_adc_full_ready = 0U;
  g_adc_error = 0U;
  if (g_adc_calibrated == 0U ||
      HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_dma_buffer,
                         MONITOR_ADC_DMA_COUNT) != HAL_OK ||
      HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID |
                    MONITOR_SAMPLE_FLAG_CURRENT_MISSING;
    (void)HAL_TIM_Base_Stop(&htim3);
    (void)HAL_ADC_Stop_DMA(&hadc1);
  }
  else
  {
    adc_capture_started = 1U;
    g_adc_capture_active = 1U;
  }
#else
  /* ADC 没有传感器存在检测；未接 ACS712 时不能把 PA0 浮空值当成电流。 */
  block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID |
                  MONITOR_SAMPLE_FLAG_CURRENT_MISSING;
#endif

  /* ===== 阶段 2：轮询三个通道的完成状态 ===== */
  /* 主循环每次迭代检查三个 done 标志：
   *   - vibration_done: mpu_capture_started == 0 || 已采集 1280 个原始样本
   *   - current_done: adc_capture_started == 0 || DMA 全传输中断已触发
   *   - temperature_done: temperature_conversion_started == 0 || 转换完成或超时
   * 全部完成或主循环超时（2.5 秒）后退出。
   */
  start_tick = HAL_GetTick();
  for (;;)
  {
    /* 检查完成状态 */
    vibration_done = (mpu_capture_started == 0U ||
                      vibration_source_count >= MONITOR_VIBRATION_SOURCE_SAMPLES) ? 1U : 0U;
#if MONITOR_CURRENT_SENSOR_ENABLED
    current_done = (adc_capture_started == 0U || g_adc_full_ready != 0U) ? 1U : 0U;
#else
    current_done = 1U;
#endif

    /* 温度：每 5ms 轮询一次 DS18B20 转换状态，800ms 超时 */
    if (temperature_done == 0U &&
        (HAL_GetTick() - temperature_poll_tick) >= 5U)
    {
      temperature_poll_tick = HAL_GetTick();
      if (DS18B20_ConversionReady() != 0U)
      {
        temperature_done = 1U;
        temperature_status = MONITORING_OK;
      }
      else if ((HAL_GetTick() - temperature_start_tick) > 800U)
      {
        temperature_done = 1U;
        temperature_status = MONITORING_ERROR_TIMEOUT;
      }
    }

    /* 电流：DMA 半传输/全传输中断触发后，在任务上下文中 memcpy 到块。
     * 中断只设置标志，不操作块池（避免所有权冲突）。 */
#if MONITOR_CURRENT_SENSOR_ENABLED
    if (adc_capture_started != 0U && g_adc_half_ready != 0U &&
        adc_half_copied == 0U)
    {
      memcpy(block->current, g_adc_dma_buffer,
             MONITOR_ADC_HALF_SAMPLES * sizeof(uint16_t));
      adc_half_copied = 1U;
      g_adc_half_ready = 0U;
    }
    if (adc_capture_started != 0U && g_adc_full_ready != 0U &&
        adc_full_copied == 0U)
    {
      memcpy(&block->current[MONITOR_ADC_HALF_SAMPLES],
             &g_adc_dma_buffer[MONITOR_ADC_HALF_SAMPLES],
             MONITOR_ADC_HALF_SAMPLES * sizeof(uint16_t));
      adc_full_copied = 1U;
      g_adc_full_ready = 0U;  /* 清零标志，保持一致性 */
    }
#endif

    /* 退出条件：三个通道都完成 */
    if (temperature_done != 0U && vibration_done != 0U && current_done != 0U)
    {
      break;
    }

    /* 退出条件：主循环超时（2.5 秒），标记超时标志并记录未完成的通道 */
    if ((HAL_GetTick() - start_tick) > MONITOR_CAPTURE_TIMEOUT_MS)
    {
      block->flags |= MONITOR_SAMPLE_FLAG_TIMEOUT;
      if (vibration_done == 0U)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID;
      }
      if (current_done == 0U)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID;
      }
      break;
    }

    /* 振动：轮询 MPU6050 FIFO
     * PE2 中断只负责唤醒采集任务；没有事件时每 1 ms 超时一次，
     * 这样既不在 ISR 中访问 I2C，也能在中断线异常时最终走超时降级。 */
    if (vibration_done == 0U)
    {
      if (MPU6050_DataReadyPending() == 0U && g_monitor_control_event != NULL)
      {
        uint32_t capture_events = osEventFlagsWait(
          g_monitor_control_event, MONITOR_EVENT_CAPTURE, osFlagsWaitAny, 1U);
        if ((capture_events & osFlagsError) != 0U ||
            (capture_events & MONITOR_EVENT_CAPTURE) == 0U)
        {
          continue;
        }
      }

      /* 读取 FIFO 计数，检查溢出和对齐 */
      if (MPU6050_ReadFifoCount(&fifo_count) != MONITORING_OK)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
        (void)MPU6050_StopCapture();
        mpu_capture_started = 0U;
      }
      else if ((fifo_count % 6U) != 0U)
      {
        /* FIFO 计数应该是 6 的倍数（X/Y/Z 各 2 字节），否则表示数据错位 */
        block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
        (void)MPU6050_StopCapture();
        mpu_capture_started = 0U;
      }
      else
      {
        /* 一次循环尽量排空 FIFO，避免 1024 字节 FIFO 在 1 kHz 原始采样下溢出。
         * 每 5 个原始样本保留前 4 个，得到 800 Hz 的等效窗口（1280 → 1024）。 */
         while (fifo_count >= 6U &&
                vibration_source_count < MONITOR_VIBRATION_SOURCE_SAMPLES)
         {
          if (MPU6050_ReadSample(&sample) != MONITORING_OK)
          {
            block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID;
            (void)MPU6050_StopCapture();
            mpu_capture_started = 0U;
            break;
          }

           /* MPU6050 合法的 DLPF 采样基准为 1 kHz。每 5 个原始样本
            * 保留前 4 个，得到 800 Hz 的等效窗口，避免使用保留配置值。 */
           if ((vibration_source_count % 5U) != 4U)
           {
             /* 防御性检查：避免缓冲区溢出 */
             if (block->vibration_sample_count < MONITOR_VIBRATION_SAMPLES)
             {
               block->vibration[0][block->vibration_sample_count] = sample.x;
               block->vibration[1][block->vibration_sample_count] = sample.y;
               block->vibration[2][block->vibration_sample_count] = sample.z;
               block->vibration_sample_count++;
             }
             else
             {
               /* 缓冲区已满，标记溢出并停止采集 */
               block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
               (void)MPU6050_StopCapture();
               mpu_capture_started = 0U;
               break;
             }
           }
           vibration_source_count++;
           fifo_count = (uint16_t)(fifo_count - 6U);
        }
      }
    }

    /* 电流：检查 DMA 错误标志，如果触发则停止采集并标记为 OVERFLOW */
#if MONITOR_CURRENT_SENSOR_ENABLED
    if (adc_capture_started != 0U && g_adc_error != 0U)
    {
      block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
      (void)HAL_TIM_Base_Stop(&htim3);
      (void)HAL_ADC_Stop_DMA(&hadc1);
      adc_capture_started = 0U;
      g_adc_capture_active = 0U;
    }
#endif

    /* 如果还有未完成的通道，让出 CPU 给其他任务（避免空转浪费电） */
    if (temperature_done == 0U || vibration_done == 0U || current_done == 0U)
    {
      osDelay(1U);
    }
  }

  /* ===== 阶段 3：收尾与有效标志设置 ===== */

  /* 振动：只有采集到完整 1024 点且无溢出，才清除 INVALID 标志 */
  if (block->vibration_sample_count == MONITOR_VIBRATION_SAMPLES)
  {
    block->flags &= ~(MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_VIB_MISSING);
  }

  /* 电流：只有半传输和全传输都成功复制且无错误，才清除 INVALID 标志 */
#if MONITOR_CURRENT_SENSOR_ENABLED
  if (adc_capture_started != 0U && g_adc_full_ready != 0U &&
      adc_half_copied != 0U && adc_full_copied != 0U &&
      g_adc_error == 0U)
  {
    /* 半传输和全传输分别复制两个半缓冲，形成固定 1024 点窗口。 */
    block->current_sample_count = MONITOR_CURRENT_SAMPLES;
    block->current_raw = MonitoringAcquisition_Average(
      block->current, MONITOR_CURRENT_SAMPLES);
    /* 检查 ADC 饱和（12 位 ADC 最大值 4095） */
    for (uint16_t i = 0U; i < MONITOR_CURRENT_SAMPLES; i++)
    {
      if (block->current[i] >= 4095U)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_SATURATION;
        break;
      }
    }
    block->flags &= ~(MONITOR_SAMPLE_FLAG_CURRENT_INVALID |
                      MONITOR_SAMPLE_FLAG_CURRENT_MISSING);
  }
#endif

  /* 温度：读取最终结果（如果转换成功但读取失败，也标记为 INVALID） */
  if (temperature_conversion_started != 0U && temperature_done == 0U)
  {
    temperature_status = MONITORING_ERROR_TIMEOUT;
    temperature_done = 1U;
  }
  if (temperature_conversion_started != 0U && temperature_done != 0U &&
      temperature_status == MONITORING_OK)
  {
    temperature_status = DS18B20_ReadTemperatureRaw(&temperature_raw);
  }
  if (temperature_conversion_started != 0U)
  {
    if (temperature_status == MONITORING_OK)
    {
      /* DS18B20 原始值的 LSB 为 1/16 摄氏度，统一转换为 0.01 摄氏度。 */
      block->temperature_centi = (int16_t)(((int32_t)temperature_raw * 100) / 16);
    }
    else
    {
      block->flags |= MONITOR_SAMPLE_FLAG_TEMP_INVALID |
                      MONITOR_SAMPLE_FLAG_TEMP_MISSING;
    }
  }

  /* 停止所有启动过的外设，避免进 Stop 前还有外设在运行 */
  if (adc_capture_started != 0U)
  {
    (void)HAL_TIM_Base_Stop(&htim3);
    (void)HAL_ADC_Stop_DMA(&hadc1);
#if MONITOR_CURRENT_SENSOR_ENABLED
    g_adc_capture_active = 0U;
#endif
  }
  if (mpu_capture_started != 0U)
  {
    (void)MPU6050_StopCapture();
  }

  /* 最终检查：如果电流样本数为 0，标记为 INVALID（防御性检查） */
  if (block->current_sample_count == 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID;
  }

  /* 如果三个通道都无效，设置 NO_SENSOR 标志（用于整体故障判断） */
  if ((block->flags & (MONITOR_SAMPLE_FLAG_TEMP_INVALID |
                       MONITOR_SAMPLE_FLAG_VIB_INVALID |
                       MONITOR_SAMPLE_FLAG_CURRENT_INVALID)) != 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_NO_SENSOR;
  }

  return block->flags;
}

/**
 * @brief  停止所有采集外设（进 Stop 前调用）
 * @return 1=成功停止，0=停止失败
 *
 * @note   Stop 门禁检查的一部分，失败时拒绝进入 Stop 模式
 * @note   只有本周期确实启动过 DMA，才检查 HAL 返回值（避免误判）
 */
uint8_t MonitoringAcquisition_Stop(void)
{
  uint8_t status = 1U;

#if MONITOR_CURRENT_SENSOR_ENABLED
  /* 只有本周期确实启动过 DMA，Stop 才检查 HAL 返回值。启动失败时
   * HAL 可能处于 READY 状态，直接 Stop_DMA 会被误判成 Stop 门禁失败。 */
  if (g_adc_capture_active != 0U && HAL_TIM_Base_Stop(&htim3) != HAL_OK)
  {
    status = 0U;
  }
  if (g_adc_capture_active != 0U && HAL_ADC_Stop_DMA(&hadc1) != HAL_OK)
  {
    status = 0U;
  }
  g_adc_capture_active = 0U;
#endif
  if (MonitoringDrivers_IsReady(DRIVER_MPU6050))
  {
    if (MPU6050_StopCapture() != MONITORING_OK)
    {
      status = 0U;
    }
  }
  return status;
}

/**
 * @brief  Stop 唤醒后恢复采集外设
 * @return 1=成功恢复，0=恢复失败
 *
 * @note   统一由 MonitoringDrivers_Resume() 管理所有传感器的恢复
 * @note   传感器缺失不会导致恢复失败（属于通道降级，不阻止下一周期运行）
 * @note   ADC 校准失败会导致恢复失败（基础外设故障）
 */
uint8_t MonitoringAcquisition_Resume(void)
{
  uint8_t result;

  /* 重新初始化 I2C（MPU6050 依赖） */
  (void)HAL_I2C_DeInit(&hi2c1);
  MX_I2C1_Init();

  /* 统一恢复所有传感器和模块 */
  result = MonitoringDrivers_Resume();

  /* 更新本地状态变量（用于采集流程判断） */
#if MONITOR_CURRENT_SENSOR_ENABLED
  g_adc_calibrated = MonitoringDrivers_IsReady(DRIVER_ADC);
  g_adc_capture_active = 0U;
#endif

  /* 传感器缺失属于通道降级，不应阻止基础外设恢复和下一周期运行。
   * 只有 ADC 校准失败才返回失败（基础外设故障）。 */
  return result;
}

/* ===== HAL 回调函数（中断上下文） ===== */

/* ===== HAL 回调函数（中断上下文） ===== */

/**
 * @brief  ADC DMA 半传输完成回调
 * @param  hadc: ADC 句柄
 * @note   中断上下文：只设置标志，不操作块池
 * @note   任务上下文中检测标志后执行 memcpy
 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc != NULL && hadc->Instance == ADC1)
  {
    g_adc_half_ready = 1U;
    MonitoringTasks_OnAdcHalfComplete();
  }
}

/**
 * @brief  ADC DMA 全传输完成回调
 * @param  hadc: ADC 句柄
 * @note   中断上下文：停止 TIM3 触发，设置标志
 * @note   停止 TIM3 避免 DMA 已满但定时器还在触发
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc != NULL && hadc->Instance == ADC1)
  {
    /* 停止新的 TIM3 触发，任务返回后再调用 HAL_ADC_Stop_DMA。 */
    __HAL_TIM_DISABLE(&htim3);
    g_adc_full_ready = 1U;
    MonitoringTasks_OnAdcComplete();
  }
}

/**
 * @brief  ADC DMA 错误回调
 * @param  hadc: ADC 句柄
 * @note   中断上下文：只设置错误标志
 * @note   任务上下文中检测后标记为 OVERFLOW 并停止采集
 */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc != NULL && hadc->Instance == ADC1)
  {
    g_adc_error = 1U;
  }
}

/**
 * @brief  GPIO EXTI 中断回调（MPU6050 数据就绪 + NRF24 IRQ）
 * @param  gpio_pin: 触发中断的引脚
 * @note   中断上下文：只发布事件或设置标志，不访问 I2C/SPI
 */
void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == MPU6050_INT_Pin)
  {
    MPU6050_NotifyDataReadyFromISR();
    MonitoringTasks_OnMpuDataReady();
  }
  else if (gpio_pin == NRF24_IRQ_Pin)
  {
    /* 无线中断只记录 pending 状态，寄存器读取留给任务上下文。 */
    MonitoringNrf24_NotifyIrqFromISR();
  }
}
