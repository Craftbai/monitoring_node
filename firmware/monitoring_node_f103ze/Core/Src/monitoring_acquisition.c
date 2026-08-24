#include "monitoring_acquisition.h"

#include "adc.h"
#include "ds18b20.h"
#include "i2c.h"
#include "mpu6050.h"
#include "monitoring_nrf24.h"
#include "tim.h"

#include <string.h>

#define MONITOR_ADC_DMA_COUNT       MONITOR_CURRENT_SAMPLES
#define MONITOR_ADC_HALF_SAMPLES    (MONITOR_ADC_DMA_COUNT / 2U)
#define MONITOR_CAPTURE_TIMEOUT_MS  2500U
#define MONITOR_VIBRATION_SOURCE_SAMPLES \
  ((MONITOR_VIBRATION_SAMPLES * 5U) / 4U)

#if MONITOR_CURRENT_SENSOR_ENABLED
static uint16_t g_adc_dma_buffer[MONITOR_ADC_DMA_COUNT]
  __attribute__((aligned(4)));
#endif
static volatile uint8_t g_adc_half_ready;
static volatile uint8_t g_adc_full_ready;
static volatile uint8_t g_adc_error;
#if MONITOR_CURRENT_SENSOR_ENABLED
static uint8_t g_adc_calibrated;
static uint8_t g_adc_capture_active;
#endif
static uint8_t g_mpu_ready;

#if MONITOR_CURRENT_SENSOR_ENABLED
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

void MonitoringAcquisition_Init(void)
{
#if MONITOR_CURRENT_SENSOR_ENABLED
  g_adc_calibrated = (HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK) ? 1U : 0U;
  g_adc_capture_active = 0U;
#endif
  g_mpu_ready = (MPU6050_Init() == MPU6050_OK) ? 1U : 0U;
}

uint32_t MonitoringAcquisition_Capture(monitor_sample_block_t *block)
{
  uint32_t start_tick;
  uint16_t fifo_count;
  mpu6050_sample_t sample;
  ds18b20_status_t temperature_status;
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

  block->flags = 0U;
  block->vibration_rate_hz = MPU6050_SAMPLE_RATE_HZ;
  block->vibration_sample_count = 0U;
  block->current_rate_hz = 1600U;
  block->current_sample_count = 0U;

  /* 先启动转换，随后并行采集其他通道，避免把 750 ms 转换时间放在前面。 */
  temperature_status = DS18B20_StartTemperatureConversion();
  temperature_conversion_started = (temperature_status == DS18B20_OK) ? 1U : 0U;
  temperature_done = (temperature_conversion_started == 0U) ? 1U : 0U;
  temperature_start_tick = HAL_GetTick();
  temperature_poll_tick = temperature_start_tick;
  if (temperature_conversion_started == 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_TEMP_INVALID | MONITOR_SAMPLE_FLAG_TEMP_MISSING;
  }

  if (g_mpu_ready == 0U || MPU6050_StartCapture() != MPU6050_OK)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_VIB_MISSING;
  }
  else
  {
    mpu_capture_started = 1U;
  }

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

  /* 振动和电流在同一个窗口内推进，温度转换同时在后台进行。 */
  start_tick = HAL_GetTick();
  for (;;)
  {
    vibration_done = (mpu_capture_started == 0U ||
                      vibration_source_count >= MONITOR_VIBRATION_SOURCE_SAMPLES) ? 1U : 0U;
#if MONITOR_CURRENT_SENSOR_ENABLED
    current_done = (adc_capture_started == 0U || g_adc_full_ready != 0U) ? 1U : 0U;
#else
    current_done = 1U;
#endif
    if (temperature_done == 0U &&
        (HAL_GetTick() - temperature_poll_tick) >= 5U)
    {
      temperature_poll_tick = HAL_GetTick();
      if (DS18B20_ConversionReady() != 0U)
      {
        temperature_done = 1U;
        temperature_status = DS18B20_OK;
      }
      else if ((HAL_GetTick() - temperature_start_tick) > 800U)
      {
        temperature_done = 1U;
        temperature_status = DS18B20_ERROR_TIMEOUT;
      }
    }
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
    }
#endif
    if (temperature_done != 0U && vibration_done != 0U && current_done != 0U)
    {
      break;
    }

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

    if (vibration_done == 0U)
    {
      /* PE2 中断只负责唤醒采集任务；没有事件时每 1 ms 超时一次，
       * 这样既不在 ISR 中访问 I2C，也能在中断线异常时最终走超时降级。 */
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

      if (MPU6050_ReadFifoCount(&fifo_count) != MPU6050_OK)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
        (void)MPU6050_StopCapture();
        mpu_capture_started = 0U;
      }
      else if ((fifo_count % 6U) != 0U)
      {
        block->flags |= MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_OVERFLOW;
        (void)MPU6050_StopCapture();
        mpu_capture_started = 0U;
      }
      else
      {
        /* 一次循环尽量排空 FIFO，避免 1024 字节 FIFO 在 1 kHz 原始
         * 采样下溢出。 */
         while (fifo_count >= 6U &&
                vibration_source_count < MONITOR_VIBRATION_SOURCE_SAMPLES)
         {
          if (MPU6050_ReadSample(&sample) != MPU6050_OK)
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
             block->vibration[0][block->vibration_sample_count] = sample.x;
             block->vibration[1][block->vibration_sample_count] = sample.y;
             block->vibration[2][block->vibration_sample_count] = sample.z;
             block->vibration_sample_count++;
           }
           vibration_source_count++;
           fifo_count = (uint16_t)(fifo_count - 6U);
        }
      }
    }

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

    if (temperature_done == 0U || vibration_done == 0U || current_done == 0U)
    {
      osDelay(1U);
    }
  }

  if (block->vibration_sample_count == MONITOR_VIBRATION_SAMPLES)
  {
    block->flags &= ~(MONITOR_SAMPLE_FLAG_VIB_INVALID | MONITOR_SAMPLE_FLAG_VIB_MISSING);
  }

#if MONITOR_CURRENT_SENSOR_ENABLED
  if (adc_capture_started != 0U && g_adc_full_ready != 0U &&
      adc_half_copied != 0U && adc_full_copied != 0U &&
      g_adc_error == 0U)
  {
    /* 半传输和全传输分别复制两个半缓冲，形成固定 1024 点窗口。 */
    block->current_sample_count = MONITOR_CURRENT_SAMPLES;
    block->current_raw = MonitoringAcquisition_Average(
      block->current, MONITOR_CURRENT_SAMPLES);
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

  if (temperature_conversion_started != 0U && temperature_done == 0U)
  {
    temperature_status = DS18B20_ERROR_TIMEOUT;
    temperature_done = 1U;
  }
  if (temperature_conversion_started != 0U && temperature_done != 0U &&
      temperature_status == DS18B20_OK)
  {
    temperature_status = DS18B20_ReadTemperatureRaw(&temperature_raw);
  }
  if (temperature_conversion_started != 0U)
  {
    if (temperature_status == DS18B20_OK)
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

  if (block->current_sample_count == 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_CURRENT_INVALID;
  }
  if ((block->flags & (MONITOR_SAMPLE_FLAG_TEMP_INVALID |
                       MONITOR_SAMPLE_FLAG_VIB_INVALID |
                       MONITOR_SAMPLE_FLAG_CURRENT_INVALID)) != 0U)
  {
    block->flags |= MONITOR_SAMPLE_FLAG_NO_SENSOR;
  }

  return block->flags;
}

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
  if (g_mpu_ready != 0U)
  {
    if (MPU6050_StopCapture() != MPU6050_OK)
    {
      status = 0U;
    }
  }
  g_mpu_ready = 0U;
  return status;
}

uint8_t MonitoringAcquisition_Resume(void)
{
  HAL_StatusTypeDef adc_calibration_status;
  mpu6050_status_t mpu_status;

  /* Stop 后先释放旧的外设状态，再重新套用 CubeMX 生成的参数。 */
  (void)HAL_I2C_DeInit(&hi2c1);
  (void)HAL_ADC_Stop_DMA(&hadc1);
  (void)HAL_ADC_DeInit(&hadc1);
  (void)HAL_TIM_Base_DeInit(&htim3);
  MX_I2C1_Init();
  MX_ADC1_Init();
#if MONITOR_CURRENT_SENSOR_ENABLED
  adc_calibration_status = HAL_ADCEx_Calibration_Start(&hadc1);
  g_adc_calibrated = (adc_calibration_status == HAL_OK) ? 1U : 0U;
  g_adc_capture_active = 0U;
#else
  /* 电流通道关闭时，ADC 只是保留配置，不应阻止 Stop 唤醒后的恢复。 */
  adc_calibration_status = HAL_OK;
#endif
  MX_TIM3_Init();
  mpu_status = MPU6050_Init();
  g_mpu_ready = (mpu_status == MPU6050_OK) ? 1U : 0U;

  /* 传感器缺失属于通道降级，不应阻止基础外设恢复和下一周期运行。 */
  return (adc_calibration_status == HAL_OK) ? 1U : 0U;
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc != NULL && hadc->Instance == ADC1)
  {
    g_adc_half_ready = 1U;
    MonitoringTasks_OnAdcHalfComplete();
  }
}

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

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc != NULL && hadc->Instance == ADC1)
  {
    g_adc_error = 1U;
  }
}

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
